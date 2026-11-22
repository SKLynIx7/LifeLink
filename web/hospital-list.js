// Hospital cards: used to choose a hospital on the request form ("select" mode) and on
// the Hospitals page ("browse" mode).
//
//   const list = HospitalList.create(container, { mode, origin, onSelect, bloodGroupFilter });
//   list.selected        -> the chosen hospital or null
//   list.refresh()       -> reload from the server
//
// Whether a hospital is open, closing soon or closed is decided by the server. Between
// refreshes (every 30 s) the countdown is updated locally every 15 s, and a hospital that
// reaches the last 30 minutes is shown as closed straight away; the server still has the
// final say when a request is sent.

const HospitalList = (() => {
  const { h, BLOOD_GROUPS, hospitalBadge } = LifeLink;

  const REFRESH_SECONDS = 30;
  const TICK_SECONDS = 15;
  const CLOSED_WITHIN_MINUTES = 30;
  const UNAVAILABLE = "This hospital is currently unavailable. Please select another nearby hospital.";
  const FACILITY_NAMES = {
    blood_bank: "Blood bank", emergency: "Emergency room", icu: "ICU", ambulance: "Ambulance",
    laboratory: "Laboratory", pharmacy: "Pharmacy", operation_theatre: "Operation theatre"
  };

  function fill(element, ...children) {
    element.replaceChildren(...children.flat().filter((child) => child !== null && child !== undefined && child !== false));
  }

  // The status shown now: the server's, with the countdown moved on by the time since it was fetched.
  function liveStatus(hospital, fetchedAt) {
    const status = hospital.status;
    if (status.minutes_until_close < 0 || !status.accepts_new_donations) return status;
    const minutes = status.minutes_until_close - Math.floor((Date.now() - fetchedAt) / 60000);
    if (minutes <= CLOSED_WITHIN_MINUTES) {
      return { code: "closed", badge: "Closed", minutes_until_close: Math.max(0, minutes), accepts_new_donations: false,
        message: "This hospital is closing soon and is currently unavailable for new donor visits." };
    }
    if (status.code === "closing_soon") {
      return Object.assign({}, status, { minutes_until_close: minutes, message: `This hospital will close in ${minutes} minutes.` });
    }
    return status;
  }

  function hoursText(hospital) {
    const days = hospital.working_days.length === 7 ? "Every day" : hospital.working_days.join(", ");
    return hospital.open_24_hours ? `${days} · open 24 hours` : `${days} · ${hospital.opening_time} to ${hospital.closing_time}`;
  }

  function create(container, options = {}) {
    const mode = options.mode || "browse";
    const state = {
      hospitals: [],
      fetchedAt: 0,
      selectedId: null,
      showClosed: false,
      bloodGroup: "",
      origin: options.origin || null,
      loading: true,
      error: "",
      radiusKm: null,
      alerts: new Set(),  // hospitals this user asked to hear about when they reopen
      alertBusy: false
    };
    const loggedIn = Boolean(LifeLink.getSession());

    // ---- Toolbar ----
    const showClosedBox = h("input", { type: "checkbox" });
    showClosedBox.addEventListener("change", () => { state.showClosed = showClosedBox.checked; refresh(); });
    const bloodFilter = options.bloodGroupFilter ? h("select", { "aria-label": "Blood group available" },
      h("option", { value: "" }, "Any blood group"), ...BLOOD_GROUPS.map((group) => h("option", { value: group }, `Has ${group}`))) : null;
    if (bloodFilter) bloodFilter.addEventListener("change", () => { state.bloodGroup = bloodFilter.value; refresh(); });
    const summary = h("p", { class: "hl-summary", "aria-live": "polite" });
    const list = h("ul", { class: "hl-list", "aria-label": "Hospitals" });
    const notice = h("p", { class: "message", hidden: true });
    container.classList.add("hospital-list");
    fill(container,
      h("div", { class: "hl-toolbar" },
        bloodFilter,
        h("label", { class: "check hl-toggle" }, showClosedBox, h("span", {}, "Show closed hospitals"))),
      summary, notice, list);

    // ---- Dialogs ----
    function openDialog(title, ...body) {
      const closeButton = h("button", { type: "button", class: "secondary" }, "Close");
      const backdrop = h("div", { class: "modal-backdrop" },
        h("div", { class: "modal hl-modal", role: "dialog", "aria-modal": "true", "aria-label": title },
          h("h2", {}, title), ...body.filter(Boolean), h("div", { class: "action-row" }, closeButton)));
      const opener = document.activeElement;
      const close = () => {
        backdrop.remove();
        document.body.classList.remove("modal-open");
        document.removeEventListener("keydown", onKey);
        if (opener && opener.focus) opener.focus();
      };
      const onKey = (event) => { if (event.key === "Escape") close(); };
      closeButton.addEventListener("click", close);
      backdrop.addEventListener("click", (event) => { if (event.target === backdrop) close(); });
      document.addEventListener("keydown", onKey);
      document.body.append(backdrop);
      document.body.classList.add("modal-open");
      closeButton.focus();
      return close;
    }

    function showUnavailable(hospital, status) {
      openDialog("Hospital unavailable",
        h("p", {}, UNAVAILABLE),
        h("p", { class: "hint" }, `${hospital.name}: ${status.message}`));
    }

    function showDetails(hospital) {
      const status = liveStatus(hospital, state.fetchedAt);
      const row = (label, value) => value ? h("div", { class: "hl-detail" }, h("dt", {}, label), h("dd", {}, value)) : null;
      let close = null;
      const selectButton = mode === "select" ? h("button", {
        type: "button", disabled: !status.accepts_new_donations,
        onclick: () => { choose(hospital); if (close) close(); }
      }, status.accepts_new_donations ? "Select this hospital" : "Unavailable") : null;
      close = openDialog(hospital.name,
        h("div", { class: "hl-status-line" }, hospitalBadge(status), h("span", {}, status.message)),
        hospital.closure_reason && !status.accepts_new_donations ? h("p", { class: "hint" }, `Reason: ${hospital.closure_reason}`) : null,
        h("dl", { class: "hl-details" },
          row("Address", [hospital.address, hospital.city, hospital.state, hospital.pincode].filter(Boolean).join(", ")),
          row("Distance", hospital.distance_km !== undefined ? `${hospital.distance_km} km from your location` : ""),
          row("Type", hospital.hospital_type),
          row("Hours", hoursText(hospital)),
          row("Phone", hospital.phone ? h("a", { href: `tel:${hospital.phone.replace(/[^0-9+]/g, "")}` }, hospital.phone) : ""),
          row("Emergency", hospital.emergency_phone ? h("a", { href: `tel:${hospital.emergency_phone.replace(/[^0-9+]/g, "")}` }, hospital.emergency_phone) : ""),
          row("Blood groups", hospital.blood_groups.join(", ")),
          row("Facilities", hospital.facilities.map((code) => FACILITY_NAMES[code] || code).join(", ")),
          row("Website", hospital.website ? h("a", { href: hospital.website, target: "_blank", rel: "noopener noreferrer" }, hospital.website) : ""),
          row("About", hospital.description)),
        h("p", {}, h("a", { href: hospital.map_url, target: "_blank", rel: "noopener noreferrer" }, "Open in OpenStreetMap ↗")),
        selectButton);
    }

    // ---- Selection ----
    function choose(hospital) {
      const status = liveStatus(hospital, state.fetchedAt);
      if (!status.accepts_new_donations) {
        showUnavailable(hospital, status);
        return;
      }
      state.selectedId = hospital.id;
      render();
      if (options.onSelect) options.onSelect(hospital);
    }

    // If the chosen hospital stops taking visits, the choice is cleared and the user told why.
    function checkSelection() {
      if (state.selectedId === null) return;
      const chosen = state.hospitals.find((hospital) => hospital.id === state.selectedId);
      const status = chosen ? liveStatus(chosen, state.fetchedAt) : null;
      if (!status || !status.accepts_new_donations) {
        state.selectedId = null;
        notice.className = "message";
        notice.textContent = `${chosen ? chosen.name : "The hospital you chose"} is no longer available. Please select another nearby hospital.`;
        notice.hidden = false;
        if (options.onSelect) options.onSelect(null);
      }
    }

    // ---- "Notify me when this hospital reopens" ----
    async function toggleReopenAlert(hospital) {
      if (state.alertBusy) return;
      state.alertBusy = true;
      const on = state.alerts.has(hospital.id);
      const result = await LifeLink.api(on ? "DELETE" : "POST", `/api/hospitals/${hospital.id}/reopen-alert`);
      state.alertBusy = false;
      notice.hidden = false;
      if (result.ok) {
        if (on) state.alerts.delete(hospital.id); else state.alerts.add(hospital.id);
        notice.className = "message success";
        notice.textContent = on ? `You will not be notified about ${hospital.name}.`
          : `${result.data.message} You will see it in the bell at the top.`;
      } else {
        notice.className = "message";
        notice.textContent = result.data.error || "Could not save that.";
        if (result.status === 409) refresh();  // it may have just reopened
      }
      render();
    }

    function reopenButton(hospital, status) {
      // Only for hospitals that are closed for now (not permanently), for logged-in users.
      if (!loggedIn || status.accepts_new_donations || status.code === "permanently_closed") return null;
      const on = state.alerts.has(hospital.id);
      return h("button", {
        type: "button", class: `hl-action link-like hl-alert ${on ? "on" : ""}`,
        "aria-pressed": on ? "true" : "false",
        onclick: () => toggleReopenAlert(hospital)
      }, on ? "Notifying you when it reopens ✓ (cancel)" : "Notify me when it reopens");
    }

    // ---- Rendering ----
    function card(hospital) {
      const status = liveStatus(hospital, state.fetchedAt);
      const available = status.accepts_new_donations;
      const selected = hospital.id === state.selectedId;
      const name = h("h3", {}, hospital.name);
      const meta = [hospital.hospital_type, hospital.distance_km !== undefined ? `${hospital.distance_km} km away` : null]
        .filter(Boolean).join(" · ");
      const actions = h("div", { class: "hl-actions" },
        hospital.phone ? h("a", { class: "hl-action", href: `tel:${hospital.phone.replace(/[^0-9+]/g, "")}` }, `Call ${hospital.phone}`) : null,
        h("a", { class: "hl-action", href: hospital.map_url, target: "_blank", rel: "noopener noreferrer" }, "Map ↗"),
        h("button", { type: "button", class: "hl-action link-like", onclick: () => showDetails(hospital) }, "View details"),
        reopenButton(hospital, status),
        mode === "select" ? h("button", {
          type: "button",
          class: `hl-select ${selected ? "chosen" : ""}`,
          "aria-disabled": available ? "false" : "true",
          "aria-pressed": selected ? "true" : "false",
          onclick: () => choose(hospital)
        }, selected ? "Selected ✓" : available ? "Select" : "Unavailable") : null);
      return h("li", {
        class: `hl-card ${available ? "" : "unavailable"} ${selected ? "selected" : ""}`,
        "data-hospital-id": String(hospital.id)
      },
        h("div", { class: "hl-head" }, name, hospitalBadge(status)),
        meta ? h("p", { class: "hl-meta" }, meta) : null,
        h("p", { class: "hl-address" }, [hospital.address, hospital.city].filter(Boolean).join(", ")),
        h("p", { class: `hl-message hl-${status.code.replace(/_/g, "-")}-text` }, status.message,
          status.code === "open" && !hospital.open_24_hours ? ` Hours: ${hospital.opening_time}–${hospital.closing_time}.` : ""),
        h("div", { class: "hl-groups", "aria-label": "Blood groups available" },
          ...hospital.blood_groups.map((group) => h("span", { class: "blood-tag" }, group))),
        actions);
    }

    function render() {
      if (state.loading && !state.hospitals.length) {
        summary.textContent = "Loading hospitals…";
        fill(list, ...[1, 2, 3].map(() => h("li", { class: "hl-card hl-skeleton", "aria-hidden": "true" },
          h("span", { class: "lp-line lp-line-long" }), h("span", { class: "lp-line lp-line-short" }), h("span", { class: "lp-line lp-line-short" }))));
        return;
      }
      if (state.error) {
        summary.textContent = "";
        fill(list, h("li", { class: "hl-empty" }, state.error, " ",
          h("button", { type: "button", class: "secondary small-button", onclick: refresh }, "Retry")));
        return;
      }
      const where = state.radiusKm ? ` within ${state.radiusKm} km of your location, nearest first` : "";
      const openCount = state.hospitals.filter((hospital) => liveStatus(hospital, state.fetchedAt).accepts_new_donations).length;
      summary.textContent = `${state.hospitals.length} hospital${state.hospitals.length === 1 ? "" : "s"}${where} · ${openCount} open now` +
        ` · updated ${new Date(state.fetchedAt).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}`;
      if (!state.hospitals.length) {
        fill(list, h("li", { class: "hl-empty" },
          state.radiusKm ? `No hospitals found within ${state.radiusKm} km of your location.` : "No hospitals found.",
          state.bloodGroup ? " Try another blood group." : "",
          !state.showClosed ? " Tick “Show closed hospitals” to include closed ones." : ""));
        return;
      }
      fill(list, ...state.hospitals.map(card));
    }

    async function refresh() {
      state.loading = true;
      if (!state.hospitals.length) render();
      const params = new URLSearchParams();
      if (state.origin) {
        params.set("latitude", state.origin.latitude);
        params.set("longitude", state.origin.longitude);
      }
      if (state.showClosed) params.set("show_closed", "1");
      if (state.bloodGroup) params.set("blood_group", state.bloodGroup);
      const [result, alerts] = await Promise.all([
        LifeLink.api("GET", `/api/hospitals/nearby?${params}`),
        loggedIn ? LifeLink.api("GET", "/api/reopen-alerts") : Promise.resolve(null)]);
      if (alerts && alerts.ok) state.alerts = new Set(alerts.data.hospital_ids);
      state.loading = false;
      if (!result.ok) {
        state.error = result.status === 0 || navigator.onLine === false
          ? "You are offline or the server is not running." : (result.data.error || "Could not load hospitals.");
        render();
        return;
      }
      state.error = "";
      state.hospitals = result.data.hospitals;
      state.radiusKm = result.data.radius_km || null;
      state.fetchedAt = Date.now();
      checkSelection();
      render();
    }

    setInterval(() => { if (!document.hidden) refresh(); }, REFRESH_SECONDS * 1000);
    setInterval(() => { if (state.hospitals.length) { checkSelection(); render(); } }, TICK_SECONDS * 1000);
    refresh();

    return {
      refresh,
      get selected() { return state.hospitals.find((hospital) => hospital.id === state.selectedId) || null; },
      setOrigin(origin) { state.origin = origin; refresh(); },
      clearSelection() { state.selectedId = null; render(); },
      showUnavailableMessage(message) {
        notice.className = "message";
        notice.textContent = message || UNAVAILABLE;
        notice.hidden = false;
        state.selectedId = null;
        refresh();
      }
    };
  }

  return { create };
})();
