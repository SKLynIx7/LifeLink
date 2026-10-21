// Profile page: change name, blood group, area, mobile number (donors) and password.

(() => {
  const { api, h, showMessage, BLOOD_GROUPS, AREAS } = LifeLink;

  const session = LifeLink.requireLogin();
  if (!session) {
    return;
  }
  LifeLink.setupHeader(session, { back: true });

  const PHONE_PATTERN = /^[6-9][0-9]{9}$/;
  const profileForm = document.getElementById("profile-form");
  const profileMessage = document.getElementById("profile-message");
  const areaSelect = document.getElementById("area");
  const locationNote = document.getElementById("location-note");
  const phoneField = document.getElementById("phone-field");

  let me = null;
  let savedLocation = null;    // the location already on the account, if it is not one of the listed areas
  let currentLocation = null;  // set by "Use my current location"

  document.getElementById("blood-type").replaceChildren(
    ...BLOOD_GROUPS.map((group) => h("option", { value: group }, group)));

  function fillAreas(selectedName) {
    const options = AREAS.map((area, index) => h("option", { value: String(index) }, area.name));
    if (savedLocation) {
      options.unshift(h("option", { value: "saved" }, `${savedLocation.name} (saved)`));
    }
    areaSelect.replaceChildren(...options);
    const index = AREAS.findIndex((area) => area.name === selectedName);
    areaSelect.value = index >= 0 ? String(index) : (savedLocation ? "saved" : "0");
  }

  async function load() {
    const result = await api("GET", "/api/me");
    if (!result.ok) {
      showMessage(profileMessage, result.data.error || "Could not load your details.");
      return;
    }
    me = result.data;
    document.getElementById("name").value = me.name;
    document.getElementById("email").value = me.email;
    document.getElementById("blood-type").value = me.blood_type;
    if (!AREAS.some((area) => area.name === me.area)) {
      // Keep a location that is not in the list (for example "Current location").
      savedLocation = { name: me.area, latitude: me.latitude, longitude: me.longitude };
    }
    fillAreas(me.area);
    phoneField.hidden = !me.is_donor;
    if (me.is_donor) {
      document.getElementById("phone").value = me.donor_profile.phone;
    }
  }

  areaSelect.addEventListener("change", () => {
    currentLocation = null;
    locationNote.textContent = "";
  });

  document.getElementById("use-location").addEventListener("click", () => {
    if (!navigator.geolocation) {
      locationNote.textContent = "Your browser cannot share its location. Please choose your area instead.";
      return;
    }
    locationNote.textContent = "Getting your location…";
    navigator.geolocation.getCurrentPosition(
      (position) => {
        currentLocation = { name: "Current location", latitude: position.coords.latitude, longitude: position.coords.longitude };
        locationNote.textContent = "Your current location will be saved.";
      },
      () => { locationNote.textContent = "Could not get your location. Please choose your area instead."; },
      { timeout: 10000 });
  });

  profileForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!me) return;
    const name = document.getElementById("name").value.trim();
    const phone = document.getElementById("phone").value.trim();
    if (!name) {
      showMessage(profileMessage, "Enter your name.");
      return;
    }
    if (me.is_donor && !PHONE_PATTERN.test(phone)) {
      showMessage(profileMessage, "Enter a valid 10-digit mobile number.");
      return;
    }

    const body = { name, blood_type: document.getElementById("blood-type").value };
    if (currentLocation) {
      Object.assign(body, { area: currentLocation.name, latitude: currentLocation.latitude, longitude: currentLocation.longitude });
    } else if (areaSelect.value === "saved") {
      // Unchanged location that is not one of the listed areas: keep it as it is.
      Object.assign(body, { area: savedLocation.name, latitude: savedLocation.latitude, longitude: savedLocation.longitude });
    } else {
      const area = AREAS[Number(areaSelect.value)];
      Object.assign(body, { area: area.name, latitude: area.latitude, longitude: area.longitude });
    }
    if (me.is_donor) body.phone = phone;

    const button = profileForm.querySelector("button[type=submit]");
    button.disabled = true;
    const result = await api("POST", "/api/profile", body);
    button.disabled = false;
    if (!result.ok) {
      showMessage(profileMessage, result.data.error || "Could not save your details.");
      return;
    }
    // Keep the name in the header in step.
    LifeLink.saveSession(Object.assign({}, LifeLink.getSession(), { name: result.data.name }));
    currentLocation = null;
    locationNote.textContent = "";
    await load();
    LifeLink.announceChange();
    showMessage(profileMessage, result.data.message, "success");
  });

  const passwordForm = document.getElementById("password-form");
  const passwordMessage = document.getElementById("password-message");
  passwordForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const current = document.getElementById("current-password").value;
    const next = document.getElementById("new-password").value;
    if (!current) {
      showMessage(passwordMessage, "Enter your current password.");
      return;
    }
    if (next.length < 8) {
      showMessage(passwordMessage, "New password must be at least 8 characters.");
      return;
    }
    if (next !== document.getElementById("confirm-password").value) {
      showMessage(passwordMessage, "The two new passwords do not match.");
      return;
    }
    const button = passwordForm.querySelector("button[type=submit]");
    button.disabled = true;
    const result = await api("POST", "/api/change-password", { current_password: current, new_password: next });
    button.disabled = false;
    if (!result.ok) {
      showMessage(passwordMessage, result.data.error || "Could not change the password.");
      return;
    }
    LifeLink.saveSession(Object.assign({}, LifeLink.getSession(), { token: result.data.session_token }));
    passwordForm.reset();
    showMessage(passwordMessage, "Password changed. You stay logged in here; other devices need the new password.", "success");
  });

  load();
})();
