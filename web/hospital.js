// Hospital side of LifeLink: login, registration and the hospital dashboard.
// A hospital account is separate from a person's account: it has its own session,
// sent as the X-Hospital-Token header.

const LifeLinkHospital = (() => {
  const { h, showMessage, BLOOD_GROUPS, hospitalBadge } = LifeLink;

  const SESSION_KEY = "lifelink.hospitalSession";
  const DASHBOARD = "/hospital-dashboard.html";
  const TYPES = ["Government", "Private", "Trust", "NGO", "Other"];
  const DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
  const FACILITIES = [
    ["blood_bank", "Blood bank"], ["emergency", "Emergency room"], ["icu", "ICU"], ["ambulance", "Ambulance"],
    ["laboratory", "Laboratory"], ["pharmacy", "Pharmacy"], ["operation_theatre", "Operation theatre"]
  ];
  const REASONS = ["Emergency maintenance", "Blood stock unavailable", "Staff unavailability", "Holiday", "Other"];

  // The same rules the server applies, so most mistakes are shown before sending.
  const EMAIL_PATTERN = /^[a-z0-9._%+\-]+@[a-z0-9\-]+(\.[a-z0-9\-]+)+$/i;
  const PHONE_PATTERN = /^\+?[0-9][0-9 \-]{8,18}[0-9]$/;
  const PINCODE_PATTERN = /^[1-9][0-9]{5}$/;
  const WEBSITE_PATTERN = /^https?:\/\/[A-Za-z0-9.\-]+(:[0-9]+)?(\/\S*)?$/;

  // ---------------- Session and API ----------------
  function getSession() {
    try {
      const saved = JSON.parse(localStorage.getItem(SESSION_KEY));
      return saved && saved.token ? saved : null;
    } catch (error) {
      return null;
    }
  }
  function saveSession(session) {
    try { localStorage.setItem(SESSION_KEY, JSON.stringify(session)); } catch (error) { /* storage blocked */ }
  }
  function clearSession() {
    try { localStorage.removeItem(SESSION_KEY); } catch (error) { /* nothing to clear */ }
  }

  async function hospitalApi(method, path, body) {
    const headers = { "Content-Type": "application/json" };
    const session = getSession();
    if (session) headers["X-Hospital-Token"] = session.token;
    let response;
    try {
      response = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
    } catch (error) {
      return { ok: false, status: 0, data: { error: "Cannot reach the LifeLink server. Is it running?" } };
    }
    let data = {};
    try { data = await response.json(); } catch (error) { data = {}; }
    if (response.status === 401 && session && !path.includes("/login")) {
      clearSession();
      window.location.href = "/hospital-login.html?expired=1";
    }
    return { ok: response.ok, status: response.status, data };
  }

  // ---------------- Inline field errors ----------------
  function clearErrors(root) {
    root.querySelectorAll(".field-error").forEach((element) => element.remove());
    root.querySelectorAll("[aria-invalid]").forEach((element) => element.removeAttribute("aria-invalid"));
  }

  // Shows each problem under its field and focuses the first one. Returns true when there were none.
  function showErrors(problems) {
    problems.forEach(({ field, text }) => {
      // The message goes inside the field's own box, directly under it, so it stays in the
      // right column of two-column rows.
      const group = field.closest(".check-group") || field.closest(".location-field");
      if (!group) field.setAttribute("aria-invalid", "true");
      const box = group || field.closest("label") || field.parentElement;
      box.append(h("p", { class: "field-error", role: "alert" }, text));
    });
    if (problems.length) {
      problems[0].field.focus();
      problems[0].field.scrollIntoView({ behavior: "smooth", block: "center" });
    }
    return problems.length === 0;
  }

  // ---------------- Shared form sections ----------------
  function checkGroup(name, options, label) {
    return h("fieldset", { class: "check-group", "data-name": name },
      h("legend", {}, label),
      h("div", { class: "check-options" },
        ...options.map(([value, text]) => h("label", { class: "check" },
          h("input", { type: "checkbox", name, value }), h("span", {}, text)))));
  }

  function checkedValues(root, name) {
    return [...root.querySelectorAll(`input[name="${name}"]:checked`)].map((input) => input.value);
  }

  function setChecked(root, name, values) {
    root.querySelectorAll(`input[name="${name}"]`).forEach((input) => { input.checked = values.includes(input.value); });
  }

  // Type, licence, blood groups, opening hours and working days.
  function renderServiceFields(container) {
    const open24 = h("input", { type: "checkbox", id: "h-24h" });
    const opening = h("input", { type: "time", id: "h-open", value: "09:00", required: true });
    const closing = h("input", { type: "time", id: "h-close", value: "20:00", required: true });
    open24.addEventListener("change", () => {
      opening.disabled = closing.disabled = open24.checked;
    });
    container.replaceChildren(
      h("div", { class: "two-col" },
        h("label", {}, "Hospital type",
          h("select", { id: "h-type", required: true },
            h("option", { value: "" }, "Choose…"), ...TYPES.map((type) => h("option", { value: type }, type)))),
        h("label", {}, "Licence / registration number (if any)",
          h("input", { id: "h-license", type: "text", maxlength: "50" }))),
      checkGroup("blood", BLOOD_GROUPS.map((group) => [group, group]), "Blood groups available"),
      h("div", { class: "hours-row" },
        h("label", {}, "Opening time", opening),
        h("label", {}, "Closing time", closing),
        h("label", { class: "check inline-check" }, open24, h("span", {}, "Open 24 hours"))),
      checkGroup("days", DAYS.map((day) => [day, day]), "Working days"));
    setChecked(container, "days", DAYS);
  }

  // Website, description, emergency number and facilities.
  function renderOptionalFields(container) {
    container.replaceChildren(
      h("div", { class: "two-col" },
        h("label", {}, "Emergency contact number", h("input", { id: "h-emergency", type: "tel", maxlength: "25" })),
        h("label", {}, "Website", h("input", { id: "h-website", type: "url", maxlength: "200", placeholder: "https://" }))),
      h("label", {}, "Description",
        h("textarea", { id: "h-description", rows: "3", maxlength: "1000", placeholder: "Services, blood bank details, how to reach…" })),
      checkGroup("facilities", FACILITIES, "Facilities"));
  }

  const field = (root, id) => root.querySelector(`#${id}`);

  // Reads the profile fields; returns { body, problems }.
  function readProfile(root) {
    const problems = [];
    const value = (id) => field(root, id).value.trim();
    const open24 = field(root, "h-24h").checked;
    const body = {
      name: value("h-name"),
      phone: value("h-phone"),
      emergency_phone: value("h-emergency"),
      hospital_type: value("h-type"),
      license_number: value("h-license"),
      blood_groups: checkedValues(root, "blood"),
      opening_time: open24 ? "00:00" : value("h-open"),
      closing_time: open24 ? "00:00" : value("h-close"),
      working_days: checkedValues(root, "days"),
      website: value("h-website"),
      description: value("h-description"),
      facilities: checkedValues(root, "facilities")
    };
    if (body.name.length < 2) problems.push({ field: field(root, "h-name"), text: "Enter the hospital name." });
    if (!PHONE_PATTERN.test(body.phone)) problems.push({ field: field(root, "h-phone"), text: "Enter a valid phone number, e.g. 0135-2654321 or 9876543210." });
    if (!body.hospital_type) problems.push({ field: field(root, "h-type"), text: "Choose the hospital type." });
    if (!body.blood_groups.length) problems.push({ field: root.querySelector('input[name="blood"]'), text: "Choose at least one blood group." });
    if (!open24 && (!body.opening_time || !body.closing_time)) problems.push({ field: field(root, "h-open"), text: "Enter opening and closing times, or tick Open 24 hours." });
    if (!open24 && body.opening_time && body.opening_time === body.closing_time) problems.push({ field: field(root, "h-close"), text: "Opening and closing times are the same. Tick Open 24 hours if you never close." });
    if (!body.working_days.length) problems.push({ field: root.querySelector('input[name="days"]'), text: "Choose at least one working day." });
    if (body.emergency_phone && !PHONE_PATTERN.test(body.emergency_phone)) problems.push({ field: field(root, "h-emergency"), text: "Enter a valid number, or leave it empty." });
    if (body.website && !WEBSITE_PATTERN.test(body.website)) problems.push({ field: field(root, "h-website"), text: "Start the website with http:// or https://." });
    return { body, problems };
  }

  function fillProfile(root, hospital) {
    field(root, "h-name").value = hospital.name;
    field(root, "h-phone").value = hospital.phone;
    field(root, "h-emergency").value = hospital.emergency_phone;
    field(root, "h-type").value = hospital.hospital_type;
    field(root, "h-license").value = hospital.license_number || "";
    setChecked(root, "blood", hospital.blood_groups);
    const open24 = hospital.open_24_hours;
    field(root, "h-24h").checked = open24;
    field(root, "h-open").value = open24 ? "09:00" : hospital.opening_time;
    field(root, "h-close").value = open24 ? "20:00" : hospital.closing_time;
    field(root, "h-open").disabled = field(root, "h-close").disabled = open24;
    setChecked(root, "days", hospital.working_days);
    field(root, "h-website").value = hospital.website;
    field(root, "h-description").value = hospital.description;
    setChecked(root, "facilities", hospital.facilities);
  }

  // Address fields plus the map location (from the location picker).
  function readAddress(root, location) {
    const problems = [];
    const value = (id) => field(root, id).value.trim();
    const body = {
      address: value("h-address"), city: value("h-city"), state: value("h-state"),
      country: value("h-country"), pincode: value("h-pincode")
    };
    if (body.address.length < 5) problems.push({ field: field(root, "h-address"), text: "Enter the hospital address." });
    if (!body.city) problems.push({ field: field(root, "h-city"), text: "Enter the city." });
    if (!body.state) problems.push({ field: field(root, "h-state"), text: "Enter the state." });
    if (!body.country) problems.push({ field: field(root, "h-country"), text: "Enter the country." });
    if (!PINCODE_PATTERN.test(body.pincode)) problems.push({ field: field(root, "h-pincode"), text: "Enter a 6-digit PIN code." });
    if (!location) {
      problems.push({ field: field(root, "choose-location"), text: "Select the hospital's exact location on the map." });
    } else {
      Object.assign(body, { latitude: location.latitude, longitude: location.longitude, place_id: location.place_id || "" });
    }
    return { body, problems };
  }

  // Fills empty address fields from the place chosen on the map (the user can still edit them).
  function fillAddressFromMap(root, location, { overwrite } = {}) {
    const set = (id, value) => {
      const input = field(root, id);
      if (value && (overwrite || !input.value.trim())) input.value = value;
    };
    const street = (location.formatted_address || "")
      .split(", ").filter((part) => ![location.city, location.state, location.country].includes(part) &&
        !part.includes(location.postal_code || "\u0000")).join(", ");
    set("h-address", street || location.area);
    set("h-city", location.city);
    set("h-state", location.state);
    set("h-country", location.country);
    set("h-pincode", location.postal_code);
  }

  function attachMapField(root, initial, onChange) {
    return LocationPicker.attachField({
      preview: field(root, "location-preview"),
      button: field(root, "choose-location"),
      note: field(root, "location-note"),
      initial,
      onChange,
      emptyText: "No location selected yet. Hospitals must select their exact location."
    });
  }

  // ---------------- Login ----------------
  function initLogin() {
    if (getSession()) {
      window.location.replace(DASHBOARD);
      return;
    }
    const form = document.getElementById("hospital-login-form");
    const message = document.getElementById("login-message");
    if (new URLSearchParams(window.location.search).get("expired")) {
      showMessage(message, "Your session has ended. Please log in again.", "info");
    }
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const email = form.querySelector("#email").value.trim();
      const password = form.querySelector("#password").value;
      if (!EMAIL_PATTERN.test(email) || !password) {
        showMessage(message, "Enter your hospital's email and password.");
        return;
      }
      const button = form.querySelector("button[type=submit]");
      button.disabled = true;
      const result = await hospitalApi("POST", "/api/hospitals/login", { email, password });
      button.disabled = false;
      if (!result.ok) {
        showMessage(message, result.data.error || "Login failed. Please try again.");
        return;
      }
      saveSession({ token: result.data.session_token, name: result.data.name, id: result.data.hospital_id });
      window.location.href = DASHBOARD;
    });
  }

  // ---------------- Registration ----------------
  function initRegister() {
    if (getSession()) {
      window.location.replace(DASHBOARD);
      return;
    }
    const form = document.getElementById("hospital-register-form");
    const message = document.getElementById("register-message");
    renderServiceFields(document.getElementById("service-fields"));
    renderOptionalFields(document.getElementById("optional-fields"));
    const mapField = attachMapField(form, null, (location) => fillAddressFromMap(form, location));

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      clearErrors(form);
      showMessage(message, "");
      const profile = readProfile(form);
      const address = readAddress(form, mapField.value);
      const email = field(form, "h-email").value.trim();
      const password = field(form, "h-password").value;
      const problems = [];
      if (!EMAIL_PATTERN.test(email)) problems.push({ field: field(form, "h-email"), text: "Enter a valid official email address." });
      if (password.length < 8 || !/[A-Za-z]/.test(password) || !/[0-9]/.test(password)) {
        problems.push({ field: field(form, "h-password"), text: "Use at least 8 characters with a letter and a number." });
      } else if (password !== field(form, "h-confirm").value) {
        problems.push({ field: field(form, "h-confirm"), text: "The two passwords do not match." });
      }
      // Problems in the order the fields appear on the form.
      const all = [...profile.problems.filter((p) => ["h-name", "h-phone"].includes(p.field.id)), ...problems,
        ...address.problems, ...profile.problems.filter((p) => !["h-name", "h-phone"].includes(p.field.id))];
      if (!showErrors(all)) {
        showMessage(message, "Please correct the highlighted fields.");
        return;
      }

      const button = form.querySelector("button[type=submit]");
      button.disabled = true;
      button.textContent = "Registering…";
      const result = await hospitalApi("POST", "/api/hospitals/register",
        Object.assign({ email, password }, profile.body, address.body));
      button.disabled = false;
      button.textContent = "Register hospital";
      if (!result.ok) {
        showMessage(message, result.data.error || "Registration failed. Please try again.");
        message.scrollIntoView({ behavior: "smooth", block: "center" });
        return;
      }
      saveSession({ token: result.data.session_token, name: result.data.name, id: result.data.hospital_id });
      window.location.href = `${DASHBOARD}?welcome=1`;
    });
  }

  // ---------------- Dashboard ----------------
  function initDashboard() {
    const session = getSession();
    if (!session) {
      window.location.replace("/hospital-login.html");
      return;
    }
    const statusBox = document.getElementById("status-now");
    const countdown = document.getElementById("countdown");
    const availabilityForm = document.getElementById("availability-form");
    const availabilityMessage = document.getElementById("availability-message");
    const profileForm = document.getElementById("profile-form");
    const profileMessage = document.getElementById("profile-message");
    const locationForm = document.getElementById("location-form");
    const locationMessage = document.getElementById("location-message");

    const verificationBanner = document.getElementById("verification-banner");
    const emergencyForm = document.getElementById("emergency-form");
    const emergencyMessage = document.getElementById("emergency-message");
    const emergencyList = document.getElementById("emergency-list");
    document.getElementById("e-blood").replaceChildren(
      h("option", { value: "" }, "Choose…"), ...BLOOD_GROUPS.map((group) => h("option", { value: group }, group)));

    let hospital = null;
    let loadedAt = 0;  // when hospital was fetched (for the countdown)
    let refreshing = false;

    renderServiceFields(document.getElementById("service-fields"));
    renderOptionalFields(document.getElementById("optional-fields"));
    const mapField = attachMapField(locationForm, null, (location) => fillAddressFromMap(locationForm, location, { overwrite: true }));

    document.getElementById("closure-reason").replaceChildren(
      h("option", { value: "" }, "No reason given"), ...REASONS.map((reason) => h("option", { value: reason }, reason)));

    document.getElementById("logout").addEventListener("click", async () => {
      await hospitalApi("POST", "/api/hospitals/logout");
      clearSession();
      window.location.href = "/hospital-login.html";
    });
    if (new URLSearchParams(window.location.search).get("welcome")) {
      showMessage(availabilityMessage, "Your hospital is registered. An administrator will check it before donors can see it; meanwhile you can set your hours and availability here.", "success");
    }

    // Shows only the controls that apply to the chosen status.
    function updateAvailabilityControls() {
      const status = availabilityForm.querySelector('input[name="status"]:checked').value;
      const type = availabilityForm.querySelector('input[name="closure-type"]:checked').value;
      document.getElementById("closure-type-group").hidden = status !== "temporarily_closed";
      document.getElementById("closing-minutes-field").hidden = status !== "temporarily_closed" || type !== "closing_soon";
      document.getElementById("reason-field").hidden = status === "open";
    }
    availabilityForm.addEventListener("change", updateAvailabilityControls);
    availabilityForm.querySelectorAll("[data-minutes]").forEach((button) => button.addEventListener("click", () => {
      document.getElementById("closing-minutes").value = button.dataset.minutes;
    }));

    function secondsLeft() {
      if (!hospital || hospital.seconds_until_manual_close === null) return null;
      return hospital.seconds_until_manual_close - Math.floor((Date.now() - loadedAt) / 1000);
    }

    function renderStatus() {
      if (!hospital) return;
      statusBox.replaceChildren(...[
        hospitalBadge(hospital.status),
        h("span", { class: "status-message" }, hospital.status.message),
        hospital.closure_reason ? h("span", { class: "hint" }, `Reason: ${hospital.closure_reason}`) : null
      ].filter(Boolean));
      document.getElementById("hospital-name").textContent = hospital.name;
      document.title = `${hospital.name} - LifeLink`;
      renderVerification();
    }

    // New hospitals are hidden from donors until an administrator approves them.
    function renderVerification() {
      const status = hospital.verification_status;
      verificationBanner.hidden = status === "verified";
      verificationBanner.className = `verification-banner ${status}`;
      if (status === "pending") {
        verificationBanner.replaceChildren(h("strong", {}, "Waiting for approval."),
          " An administrator checks every new hospital. Until then your hospital is not shown to donors and cannot post urgent needs.");
      } else if (status === "rejected") {
        verificationBanner.replaceChildren(h("strong", {}, "Registration not approved."),
          ` Reason: ${hospital.verification_note || "not given"}. Correct your details below and save; your hospital is then checked again.`);
      }
      const fields = document.getElementById("emergency-fields");
      fields.disabled = status !== "verified" || hospital.availability_state === "permanently_closed";
      fields.title = fields.disabled ? "Available once your hospital is approved and not permanently closed" : "";
    }

    // ---- Urgent blood needs ----
    async function loadPosts() {
      const result = await hospitalApi("GET", "/api/hospital/emergencies");
      if (!result.ok) return;
      const posts = result.data.posts;
      if (!posts.length) {
        emergencyList.replaceChildren(h("p", { class: "muted small-text" }, "No urgent needs posted in the last 7 days."));
        return;
      }
      emergencyList.replaceChildren(h("p", { class: "section-label" }, "Your recent posts"),
        ...posts.map((post) => h("div", { class: `emergency-row ${post.active ? "" : "inactive"}` },
          h("div", {},
            h("span", { class: `urgency-badge ${post.urgency}` }, post.urgency === "critical" ? "Critical" : "Urgent"),
            h("strong", {}, ` ${post.units} × ${post.blood_type}`),
            h("p", { class: "hint" }, post.active
              ? `Posted ${LifeLink.timeAgo(post.created_at)} · until ${LifeLink.formatTime(post.expires_at)}`
              : `Closed or expired · posted ${LifeLink.formatTime(post.created_at)}`),
            post.note ? h("p", { class: "small-text" }, post.note) : ""),
          post.active ? h("button", { type: "button", class: "secondary small-button", onclick: () => closePost(post) }, "Close") : "")));
    }

    async function closePost(post) {
      if (!window.confirm(`Close the ${post.blood_type} post? Donors will stop seeing it.`)) return;
      const result = await hospitalApi("POST", `/api/hospitals/${session.id}/emergencies/${post.id}/close`);
      showMessage(emergencyMessage, result.ok ? "Post closed." : (result.data.error || "Could not close the post."),
        result.ok ? "success" : "error");
      loadPosts();
    }

    emergencyForm.addEventListener("submit", async (event) => {
      event.preventDefault();
      clearErrors(emergencyForm);
      const blood = document.getElementById("e-blood");
      const units = document.getElementById("e-units");
      const problems = [];
      if (!blood.value) problems.push({ field: blood, text: "Choose the blood group needed." });
      const count = Number(units.value);
      if (!Number.isInteger(count) || count < 1 || count > 20) problems.push({ field: units, text: "Enter 1 to 20 units." });
      if (!showErrors(problems)) return;
      const button = emergencyForm.querySelector("button[type=submit]");
      button.disabled = true;
      const result = await hospitalApi("POST", `/api/hospitals/${session.id}/emergencies`, {
        blood_type: blood.value,
        units: count,
        urgency: emergencyForm.querySelector('input[name="urgency"]:checked').value,
        hours: Number(document.getElementById("e-hours").value),
        note: document.getElementById("e-note").value.trim()
      });
      button.disabled = false;
      showMessage(emergencyMessage, result.ok ? result.data.message : (result.data.error || "Could not post."),
        result.ok ? "success" : "error");
      if (result.ok) {
        document.getElementById("e-note").value = "";
        loadPosts();
      }
    });

    // Live countdown to the closing time the hospital set. When it reaches the 30-minute
    // mark the page asks the server again, which then reports the hospital as closed.
    function tick() {
      const left = secondsLeft();
      if (left === null || hospital.availability_state !== "closing_soon") {
        countdown.hidden = true;
        return;
      }
      const clamped = Math.max(0, left);
      const hh = String(Math.floor(clamped / 3600)).padStart(2, "0");
      const mm = String(Math.floor((clamped % 3600) / 60)).padStart(2, "0");
      const ss = String(clamped % 60).padStart(2, "0");
      countdown.hidden = false;
      countdown.replaceChildren(h("span", {}, "Closes in "), h("strong", {}, `${hh}:${mm}:${ss}`),
        h("span", { class: "hint" }, " New donor visits stop 30 minutes before closing."));
      const crossed = (hospital.status.code === "closing_soon" && left <= 30 * 60) || (hospital.status.code === "closed" && left <= 0);
      if (crossed && !refreshing) load();
    }

    function fillAvailability() {
      const state = hospital.availability_state;
      const status = state === "closing_soon" ? "temporarily_closed" : state;
      availabilityForm.querySelector(`input[name="status"][value="${status}"]`).checked = true;
      // "Closing soon" is offered first; "Closed now" stays selected when that is the current state.
      availabilityForm.querySelector(`input[name="closure-type"][value="${state === "temporarily_closed" ? "closed_now" : "closing_soon"}"]`).checked = true;
      const left = secondsLeft();
      document.getElementById("closing-minutes").value = state === "closing_soon" && left > 0 ? Math.max(1, Math.ceil(left / 60)) : 60;
      document.getElementById("closure-reason").value = hospital.closure_reason || "";
      updateAvailabilityControls();
    }

    async function load({ fillForms } = {}) {
      refreshing = true;
      const result = await hospitalApi("GET", "/api/hospital/me");
      refreshing = false;
      if (!result.ok) {
        showMessage(availabilityMessage, result.data.error || "Could not load your hospital.");
        return;
      }
      hospital = result.data.hospital;
      loadedAt = Date.now();
      renderStatus();
      tick();
      if (fillForms) {
        fillAvailability();
        fillProfile(profileForm, hospital);
        document.getElementById("h-email").value = hospital.email;
        const location = {
          area: hospital.name, latitude: hospital.latitude, longitude: hospital.longitude,
          formatted_address: [hospital.address, hospital.city, hospital.state, hospital.pincode].filter(Boolean).join(", "),
          place_id: hospital.place_id
        };
        mapField.value = location;
        ["address", "city", "state", "country", "pincode"].forEach((key) => {
          field(locationForm, `h-${key}`).value = hospital[key];
        });
      }
    }

    availabilityForm.addEventListener("submit", async (event) => {
      event.preventDefault();
      clearErrors(availabilityForm);
      const status = availabilityForm.querySelector('input[name="status"]:checked').value;
      const body = { status, reason: status === "open" ? "" : document.getElementById("closure-reason").value };
      if (status === "temporarily_closed") {
        body.closure_type = availabilityForm.querySelector('input[name="closure-type"]:checked').value;
        if (body.closure_type === "closing_soon") {
          const minutesInput = document.getElementById("closing-minutes");
          const minutes = Number(minutesInput.value);
          if (!Number.isInteger(minutes) || minutes < 1 || minutes > 720) {
            showErrors([{ field: minutesInput, text: "Enter whole minutes from 1 to 720 (12 hours)." }]);
            return;
          }
          body.closing_minutes = minutes;
        }
      }
      if (status === "permanently_closed" &&
          !window.confirm("Mark the hospital as permanently closed? It will be hidden from search results until you reopen it.")) {
        return;
      }
      const button = availabilityForm.querySelector("button[type=submit]");
      button.disabled = true;
      const result = await hospitalApi("PUT", `/api/hospitals/${session.id}/availability`, body);
      button.disabled = false;
      if (!result.ok) {
        showMessage(availabilityMessage, result.data.error || "Could not update availability.");
        return;
      }
      hospital = result.data.hospital;
      loadedAt = Date.now();
      renderStatus();
      tick();
      fillAvailability();
      showMessage(availabilityMessage, `Saved. Donors now see: ${hospital.status.badge} — ${hospital.status.message}`, "success");
    });

    profileForm.addEventListener("submit", async (event) => {
      event.preventDefault();
      clearErrors(profileForm);
      const profile = readProfile(profileForm);
      if (!showErrors(profile.problems)) {
        showMessage(profileMessage, "Please correct the highlighted fields.");
        return;
      }
      const button = profileForm.querySelector("button[type=submit]");
      button.disabled = true;
      const result = await hospitalApi("PUT", `/api/hospitals/${session.id}/profile`, profile.body);
      button.disabled = false;
      if (!result.ok) {
        showMessage(profileMessage, result.data.error || "Could not save the details.");
        return;
      }
      hospital = result.data.hospital;
      loadedAt = Date.now();
      renderStatus();
      saveSession(Object.assign({}, session, { name: hospital.name }));
      showMessage(profileMessage, "Hospital details saved.", "success");
    });

    locationForm.addEventListener("submit", async (event) => {
      event.preventDefault();
      clearErrors(locationForm);
      const address = readAddress(locationForm, mapField.value);
      if (!showErrors(address.problems)) {
        showMessage(locationMessage, "Please correct the highlighted fields.");
        return;
      }
      const button = locationForm.querySelector("button[type=submit]");
      button.disabled = true;
      const result = await hospitalApi("PUT", `/api/hospitals/${session.id}/location`, address.body);
      button.disabled = false;
      showMessage(locationMessage, result.ok ? "Location saved. Distances to your hospital now use it."
        : (result.data.error || "Could not save the location."), result.ok ? "success" : "error");
    });

    load({ fillForms: true });
    loadPosts();
    setInterval(tick, 1000);
    // Ask the server again every 30 seconds (regular hours can open or close the hospital too).
    setInterval(() => { if (!document.hidden) { load(); loadPosts(); } }, 30000);
  }

  return { initLogin, initRegister, initDashboard, getSession };
})();
