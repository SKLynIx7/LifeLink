// Map location picker (OpenStreetMap + Leaflet), used on Register and Profile.
//
//   LocationPicker.open({ initial }) -> Promise of a location, or null if closed.
//   LocationPicker.attachField({ preview, button, select, note, initial }) wires a form field.
//
// A location is { area, latitude, longitude, formatted_address, city, state, country,
// postal_code, place_id }: the same fields the server accepts.
//
// The map stays still under a fixed centre pin: the user drags the map, searches for a
// place, or uses the device location. Map tiles come from OpenStreetMap and place search /
// addresses from Photon (komoot), a free OpenStreetMap geocoder. No API key is needed.

const LocationPicker = (() => {
  const { h, AREAS } = LifeLink;

  // ---- Configuration ----
  const DEFAULT_CENTER = { latitude: 30.3165, longitude: 78.0322 };  // Dehradun
  const SERVICE_AREA_KM = 40;  // LifeLink lists Dehradun hospitals only
  const TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
  const SEARCH_URL = "https://photon.komoot.io/api/";
  const REVERSE_URL = "https://photon.komoot.io/reverse";
  const ATTRIBUTION = '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors' +
    ' · Search by <a href="https://photon.komoot.io">Photon</a>';
  const REQUEST_TIMEOUT_MS = 8000;
  const SEARCH_DELAY_MS = 350;   // wait for a pause in typing before searching
  const ADDRESS_DELAY_MS = 500;  // wait for the map to settle before looking up the address

  // ---- Small helpers ----
  function distanceKm(a, b) {
    const toRadians = Math.PI / 180;
    const dLat = (b.latitude - a.latitude) * toRadians;
    const dLon = (b.longitude - a.longitude) * toRadians;
    const x = Math.sin(dLat / 2) ** 2 +
      Math.cos(a.latitude * toRadians) * Math.cos(b.latitude * toRadians) * Math.sin(dLon / 2) ** 2;
    return 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(x)));
  }

  // replaceChildren would print null as the text "null", so leave empty pieces out.
  function fill(element, ...children) {
    element.replaceChildren(...children.flat().filter((child) => child !== null && child !== undefined && child !== false));
  }

  function round(value) {
    return Math.round(value * 1e6) / 1e6;
  }

  async function fetchJson(url, signal) {
    const timeout = new AbortController();
    const timer = setTimeout(() => timeout.abort(), REQUEST_TIMEOUT_MS);
    const onAbort = () => timeout.abort();
    if (signal) signal.addEventListener("abort", onAbort);
    try {
      const response = await fetch(url, { signal: timeout.signal });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      return await response.json();
    } finally {
      clearTimeout(timer);
      if (signal) signal.removeEventListener("abort", onAbort);
    }
  }

  // Leaflet is loaded only when the picker first opens, so other pages never need it.
  let leafletPromise = null;
  function loadLeaflet() {
    if (window.L) return Promise.resolve(window.L);
    if (leafletPromise) return leafletPromise;
    leafletPromise = new Promise((resolve, reject) => {
      if (!document.querySelector("link[data-leaflet]")) {
        document.head.append(h("link", { rel: "stylesheet", href: "/vendor/leaflet/leaflet.css", "data-leaflet": "true" }));
      }
      const script = h("script", { src: "/vendor/leaflet/leaflet.js" });
      script.onload = () => (window.L ? resolve(window.L) : reject(new Error("Leaflet did not load")));
      script.onerror = () => { script.remove(); reject(new Error("Leaflet did not load")); };
      document.head.append(script);
    }).catch((error) => {
      leafletPromise = null;  // allow a retry
      throw error;
    });
    return leafletPromise;
  }

  // Turns a Photon (GeoJSON) feature into a LifeLink location.
  function locationFromFeature(feature, latitude, longitude) {
    const p = feature.properties || {};
    const street = [p.housenumber, p.street].filter(Boolean).join(" ");
    const locality = p.district || p.locality || p.suburb || "";
    const city = p.city || p.town || p.village || p.county || "";
    const name = p.name && p.name !== street ? p.name : "";
    const parts = [name, street, locality, city, [p.state, p.postcode].filter(Boolean).join(" "), p.country];
    const seen = new Set();
    const formatted = parts.filter((part) => {
      if (!part || seen.has(part.toLowerCase())) return false;
      seen.add(part.toLowerCase());
      return true;
    }).join(", ");
    const short = [name || p.street || locality, city].filter(Boolean);
    const area = (short.length ? [...new Set(short)].join(", ") : formatted || "Pinned location").slice(0, 80);
    const osmType = { N: "N", W: "W", R: "R", node: "N", way: "W", relation: "R" }[p.osm_type] || "";
    return {
      area,
      latitude: round(latitude),
      longitude: round(longitude),
      formatted_address: (formatted || area).slice(0, 300),
      city: city.slice(0, 100),
      state: (p.state || "").slice(0, 100),
      country: (p.country || "").slice(0, 100),
      postal_code: (p.postcode || "").slice(0, 20),
      place_id: p.osm_id ? `${osmType}${p.osm_id}` : ""
    };
  }

  // A location with only coordinates, used when the address lookup is unavailable.
  function pinnedLocation(latitude, longitude) {
    const coordinates = `${latitude.toFixed(5)}, ${longitude.toFixed(5)}`;
    return {
      area: "Pinned location", latitude: round(latitude), longitude: round(longitude),
      formatted_address: `Pinned location (${coordinates})`,
      city: "", state: "", country: "", postal_code: "", place_id: ""
    };
  }

  // An entry of the Dehradun area list as a location.
  function locationFromArea(area) {
    return {
      area: area.name, latitude: area.latitude, longitude: area.longitude,
      formatted_address: `${area.name}, Dehradun, Uttarakhand, India`,
      city: "Dehradun", state: "Uttarakhand", country: "India", postal_code: "", place_id: ""
    };
  }

  // ---------------- The picker dialog ----------------
  function open(options = {}) {
    return new Promise((resolve) => {
      const opener = document.activeElement;
      const initial = options.initial && Number.isFinite(options.initial.latitude) ? options.initial : null;

      let map = null;
      let selected = initial;        // the location the card shows
      let addressController = null;  // aborts an older address lookup
      let searchController = null;
      let addressTimer = null;
      let searchTimer = null;
      let skipNextLookup = false;    // set when we already know the address (search result, initial)
      let activeIndex = -1;
      let results = [];
      let tileLoaded = false;
      let tileErrors = 0;
      let closed = false;

      // ---- Elements ----
      const searchInput = h("input", {
        type: "search", class: "lp-search-input", placeholder: "Search area, street or landmark",
        autocomplete: "off", "aria-label": "Search for a place", role: "combobox",
        "aria-expanded": "false", "aria-controls": "lp-results", "aria-autocomplete": "list"
      });
      const resultList = h("ul", { id: "lp-results", class: "lp-results", role: "listbox", hidden: true });
      // Two notices: one about the device location, one about the map itself.
      const banner = h("div", { class: "lp-banner", role: "status", "aria-live": "polite", hidden: true });
      const mapNotice = h("div", { class: "lp-banner lp-banner-map", role: "alert", hidden: true });
      const notices = h("div", { class: "lp-notices" }, mapNotice, banner);
      const mapBox = h("div", { class: "lp-map", "aria-label": "Map. Drag to move the pin." });
      const skeleton = h("div", { class: "lp-map-skeleton", "aria-hidden": "true" });
      const mapError = h("div", { class: "lp-map-error", hidden: true });
      const pin = h("div", { class: "lp-pin", "aria-hidden": "true" }, h("span", { class: "lp-pin-head" }), h("span", { class: "lp-pin-dot" }));
      const locateButton = h("button", { type: "button", class: "lp-locate", title: "Use my current location" },
        h("span", { class: "lp-locate-icon", "aria-hidden": "true" }), "Use my current location");
      const addressBox = h("div", { class: "lp-address", "aria-live": "polite" });
      const areaNote = h("p", { class: "lp-note", hidden: true });
      const confirmButton = h("button", { type: "button", class: "lp-confirm", disabled: true }, "Confirm location");
      const changeButton = h("button", { type: "button", class: "secondary lp-change" }, "Change location");
      const closeButton = h("button", { type: "button", class: "lp-close", "aria-label": "Close" }, "×");

      const dialog = h("div", { class: "lp-dialog", role: "dialog", "aria-modal": "true", "aria-labelledby": "lp-title" },
        h("div", { class: "lp-top" },
          h("div", { class: "lp-title-row" },
            h("h2", { id: "lp-title" }, options.title || "Select location"),
            closeButton),
          h("div", { class: "lp-search" }, searchInput, resultList)),
        h("div", { class: "lp-map-wrap" }, mapBox, skeleton, pin, mapError, locateButton, notices),
        h("div", { class: "lp-card" },
          h("p", { class: "lp-card-label" }, "Selected location"),
          addressBox, areaNote,
          h("div", { class: "lp-card-actions" }, changeButton, confirmButton)));
      const overlay = h("div", { class: "lp-overlay" }, dialog);

      // ---- Messages and states ----
      function setNotice(element, text, action) {
        fill(element, text, action ? h("button", { type: "button", class: "lp-link", onclick: action.run }, action.label) : null);
        element.hidden = !text;
      }
      const showBanner = (text, action) => setNotice(banner, text, action);
      const showMapNotice = (text, action) => setNotice(mapNotice, text, action);

      function showAddressLoading() {
        confirmButton.disabled = true;
        fill(addressBox, 
          h("span", { class: "lp-line lp-line-long" }), h("span", { class: "lp-line lp-line-short" }));
        addressBox.setAttribute("aria-busy", "true");
      }

      function showAddress(location, problem) {
        selected = location;
        addressBox.removeAttribute("aria-busy");
        fill(addressBox, 
          h("strong", {}, location.area),
          h("span", {}, location.formatted_address),
          problem ? h("span", { class: "lp-address-problem" }, problem.text, " ",
            h("button", { type: "button", class: "lp-link", onclick: problem.retry }, "Retry")) : null);
        const away = distanceKm(location, DEFAULT_CENTER);
        areaNote.hidden = away <= SERVICE_AREA_KM;
        areaNote.textContent = `This is about ${Math.round(away)} km from Dehradun. LifeLink currently lists ` +
          "Dehradun hospitals only, so donors and hospitals may not be found here.";
        confirmButton.disabled = false;
      }

      function showMapError(text) {
        skeleton.hidden = true;
        mapError.hidden = false;
        fill(mapError, 
          h("p", {}, text),
          h("button", { type: "button", class: "secondary", onclick: () => { mapError.hidden = true; start(); } }, "Retry"));
      }

      // ---- Address lookup for the point under the pin ----
      async function lookUpAddress() {
        if (!map || closed) return;  // the picker may have closed while waiting
        const center = map.getCenter();
        if (addressController) addressController.abort();
        const controller = new AbortController();
        addressController = controller;
        const url = `${REVERSE_URL}?lat=${center.lat}&lon=${center.lng}&lang=en&limit=1`;
        try {
          const data = await fetchJson(url, controller.signal);
          if (controller.signal.aborted) return;
          const feature = data && data.features && data.features[0];
          showAddress(feature ? locationFromFeature(feature, center.lat, center.lng) : pinnedLocation(center.lat, center.lng));
        } catch (error) {
          if (controller.signal.aborted) return;  // replaced by a newer lookup
          showAddress(pinnedLocation(center.lat, center.lng), {
            text: navigator.onLine === false ? "You are offline, so the address could not be found." : "Couldn't fetch the address.",
            retry: () => { showAddressLoading(); lookUpAddress(); }
          });
        }
      }

      function onMapMoved() {
        if (closed) return;
        pin.classList.remove("lp-pin-lifted");
        clearTimeout(addressTimer);
        if (skipNextLookup) {
          skipNextLookup = false;
          return;
        }
        showAddressLoading();
        addressTimer = setTimeout(lookUpAddress, ADDRESS_DELAY_MS);
      }

      // ---- Device location ----
      function useDeviceLocation({ quiet } = {}) {
        if (!("geolocation" in navigator)) {
          showBanner("Location permission is unavailable. Please search for your area or drag the pin to select your location.");
          return;
        }
        locateButton.disabled = true;
        locateButton.classList.add("lp-busy");
        if (!quiet) showBanner("Finding your location…");
        navigator.geolocation.getCurrentPosition(
          (position) => {
            if (closed) return;
            locateButton.disabled = false;
            locateButton.classList.remove("lp-busy");
            showBanner("");
            if (map) map.setView([position.coords.latitude, position.coords.longitude], 16, { animate: false });
          },
          (error) => {
            if (closed) return;
            locateButton.disabled = false;
            locateButton.classList.remove("lp-busy");
            showBanner(error.code === error.PERMISSION_DENIED
              ? "Location permission is unavailable. Please search for your area or drag the pin to select your location."
              : "Couldn't get your location. Search for your area or drag the pin instead.",
              error.code === error.PERMISSION_DENIED ? null : { label: "Try again", run: () => useDeviceLocation() });
          },
          { enableHighAccuracy: true, timeout: 10000, maximumAge: 60000 });
      }

      // ---- Place search ----
      function setResults(items, message) {
        results = items;
        activeIndex = -1;
        fill(resultList, 
          ...items.map((item, index) => h("li", {
            id: `lp-option-${index}`, role: "option", class: "lp-result",
            onmousedown: (event) => event.preventDefault(),  // keep focus in the input
            onclick: () => choose(index)
          }, h("strong", {}, item.area), h("span", {}, item.formatted_address))),
          message ? h("li", { class: "lp-result-message", role: "presentation" }, ...[].concat(message)) : null);
        const show = items.length > 0 || Boolean(message);
        resultList.hidden = !show;
        searchInput.setAttribute("aria-expanded", show ? "true" : "false");
        searchInput.removeAttribute("aria-activedescendant");
      }

      function highlight(index) {
        activeIndex = index;
        [...resultList.querySelectorAll(".lp-result")].forEach((item, i) => item.classList.toggle("active", i === index));
        if (index >= 0) searchInput.setAttribute("aria-activedescendant", `lp-option-${index}`);
      }

      function choose(index) {
        const place = results[index];
        if (!place) return;
        setResults([]);
        searchInput.value = place.area;
        skipNextLookup = true;
        showAddress(place);
        if (map) map.setView([place.latitude, place.longitude], 16, { animate: false });
      }

      async function search(query) {
        if (searchController) searchController.abort();
        const controller = new AbortController();
        searchController = controller;
        const center = map ? map.getCenter() : { lat: DEFAULT_CENTER.latitude, lng: DEFAULT_CENTER.longitude };
        const url = `${SEARCH_URL}?q=${encodeURIComponent(query)}&limit=5&lang=en&lat=${center.lat}&lon=${center.lng}`;
        setResults([], [h("span", { class: "spinner", "aria-hidden": "true" }), " Searching…"]);
        try {
          const data = await fetchJson(url, controller.signal);
          if (controller.signal.aborted) return;
          const places = (data.features || []).map((feature) =>
            locationFromFeature(feature, feature.geometry.coordinates[1], feature.geometry.coordinates[0]));
          setResults(places, places.length ? null : `No places found for “${query}”. Try another name.`);
        } catch (error) {
          if (controller.signal.aborted) return;  // replaced by a newer search
          setResults([], [navigator.onLine === false ? "You are offline. " : "Search is unavailable right now. ",
            h("button", { type: "button", class: "lp-link", onmousedown: (event) => event.preventDefault(),
              onclick: () => search(query) }, "Retry")]);
        }
      }

      searchInput.addEventListener("input", () => {
        clearTimeout(searchTimer);
        const query = searchInput.value.trim();
        if (query.length < 3) {
          if (searchController) searchController.abort();
          setResults([], query.length ? "Type at least 3 letters." : null);
          return;
        }
        searchTimer = setTimeout(() => search(query), SEARCH_DELAY_MS);
      });
      searchInput.addEventListener("keydown", (event) => {
        if (event.key === "ArrowDown" && results.length) {
          event.preventDefault();
          highlight((activeIndex + 1) % results.length);
        } else if (event.key === "ArrowUp" && results.length) {
          event.preventDefault();
          highlight((activeIndex - 1 + results.length) % results.length);
        } else if (event.key === "Enter") {
          event.preventDefault();
          if (activeIndex >= 0) choose(activeIndex);
          else if (results.length) choose(0);
        } else if (event.key === "Escape" && !resultList.hidden) {
          event.stopPropagation();
          setResults([]);
        }
      });
      searchInput.addEventListener("blur", () => setTimeout(() => setResults([]), 150));

      // ---- Opening and closing ----
      function close(result) {
        if (closed) return;
        closed = true;
        clearTimeout(addressTimer);
        clearTimeout(searchTimer);
        if (addressController) addressController.abort();
        if (searchController) searchController.abort();
        if (map) {
          // Stop any pan or zoom still running, or Leaflet errors when the map is removed mid-move.
          try { map.stop(); map.remove(); } catch (error) { /* already gone */ }
        }
        overlay.remove();
        document.body.classList.remove("lp-open");
        document.removeEventListener("keydown", onKeydown);
        if (opener && opener.focus) opener.focus();
        resolve(result);
      }

      function onKeydown(event) {
        if (event.key === "Escape") {
          close(null);
        } else if (event.key === "Tab") {
          // Keep keyboard focus inside the dialog.
          const focusable = [...dialog.querySelectorAll("button:not([disabled]), input, [tabindex]:not([tabindex='-1'])")]
            .filter((element) => element.offsetParent !== null);
          if (!focusable.length) return;
          const first = focusable[0];
          const last = focusable[focusable.length - 1];
          if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
          else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
        }
      }

      async function start() {
        skeleton.hidden = false;
        try {
          const L = await loadLeaflet();
          if (closed) return;
          const startAt = initial || DEFAULT_CENTER;
          map = L.map(mapBox, { zoomControl: false, attributionControl: true })
            .setView([startAt.latitude, startAt.longitude], initial ? 16 : 13);
          map.attributionControl.setPrefix('<a href="https://leafletjs.com">Leaflet</a>');
          L.control.zoom({ position: "bottomleft" }).addTo(map);  // keeps the top free for messages
          // OpenStreetMap blocks tile requests that do not say which site they come from.
          // The rest of LifeLink sends no referrer (privacy); the tiles send only the site's
          // origin (e.g. http://127.0.0.1:8080/), never the page path.
          const tiles = L.tileLayer(TILE_URL, {
            maxZoom: 19, attribution: ATTRIBUTION, referrerPolicy: "strict-origin-when-cross-origin"
          });
          tiles.on("tileload", () => {
            tileLoaded = true;
            skeleton.hidden = true;
            if (!mapNotice.hidden) showMapNotice("");
          });
          tiles.on("tileerror", () => {
            tileErrors += 1;
            if (!tileLoaded && tileErrors >= 4) {
              skeleton.hidden = true;
              showMapNotice(navigator.onLine === false
                ? "You are offline, so the map can't load. You can still search or use your current location. "
                : "The map could not load. ", { label: "Retry", run: () => { tileErrors = 0; showMapNotice(""); tiles.redraw(); } });
            }
          });
          tiles.addTo(map);
          map.on("movestart", () => pin.classList.add("lp-pin-lifted"));
          map.on("moveend", onMapMoved);
          if (initial) {
            showAddress(initial);
          } else {
            showAddressLoading();
            lookUpAddress();
            useDeviceLocation({ quiet: false });
          }
        } catch (error) {
          showMapError("The map could not be loaded. Check your internet connection and try again.");
        }
      }

      closeButton.addEventListener("click", () => close(null));
      overlay.addEventListener("mousedown", (event) => { if (event.target === overlay) close(null); });
      locateButton.addEventListener("click", () => useDeviceLocation());
      changeButton.addEventListener("click", () => { searchInput.focus(); searchInput.select(); });
      confirmButton.addEventListener("click", () => { if (selected) close(selected); });
      document.addEventListener("keydown", onKeydown);

      document.body.append(overlay);
      document.body.classList.add("lp-open");
      if (navigator.onLine === false) {
        showMapNotice("You are offline. The map needs an internet connection. ", { label: "Retry", run: () => { showMapNotice(""); if (!map) start(); } });
      }
      searchInput.focus();
      start();
    });
  }

  // ---------------- A location field on a form ----------------
  // preview: element that shows the chosen place; button: opens the map;
  // select: optional Dehradun area list (a fallback that needs no internet).
  // onChange(location) is called after the user picks a new location.
  function attachField({ preview, button, select, note, initial, onChange, emptyText }) {
    let current = initial || null;

    function render() {
      if (!current) {
        fill(preview, h("span", { class: "muted" }, emptyText || "No location chosen yet"));
        preview.classList.add("empty");
      } else {
        preview.classList.remove("empty");
        fill(preview, 
          h("span", { class: "location-pin", "aria-hidden": "true" }),
          h("span", {}, h("strong", {}, current.area),
            current.formatted_address && current.formatted_address !== current.area
              ? h("small", {}, current.formatted_address) : null));
      }
      button.textContent = current ? "Change on map" : "Choose on map";
    }

    if (select) {
      select.replaceChildren(
        h("option", { value: "" }, "Pick an area instead…"),
        ...AREAS.map((area, index) => h("option", { value: String(index) }, area.name)));
      select.addEventListener("change", () => {
        if (select.value === "") return;
        current = locationFromArea(AREAS[Number(select.value)]);
        if (note) note.textContent = "Using the centre of this area. Use the map for your exact spot.";
        render();
        if (onChange) onChange(current);
      });
    }

    button.addEventListener("click", async () => {
      const result = await open({ initial: current });
      if (result) {
        current = result;
        if (select) select.value = "";
        if (note) note.textContent = "Location set from the map.";
        render();
        if (onChange) onChange(current);
      }
    });

    render();
    return {
      get value() { return current; },
      set value(location) { current = location; if (select) select.value = ""; render(); }
    };
  }

  return { open, attachField, locationFromArea };
})();
