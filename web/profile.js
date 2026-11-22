// Profile page: change name, blood group, location, mobile number (donors) and password.

(() => {
  const { api, h, showMessage, BLOOD_GROUPS } = LifeLink;

  const session = LifeLink.requireLogin();
  if (!session) {
    return;
  }
  LifeLink.setupHeader(session, { back: true });

  const PHONE_PATTERN = /^[6-9][0-9]{9}$/;
  const profileForm = document.getElementById("profile-form");
  const profileMessage = document.getElementById("profile-message");
  const phoneField = document.getElementById("phone-field");
  const locationNote = document.getElementById("location-note");
  const LOCATION_HINT = locationNote.textContent;

  let me = null;
  const locationField = LocationPicker.attachField({
    preview: document.getElementById("location-preview"),
    button: document.getElementById("choose-location"),
    select: document.getElementById("area"),
    note: locationNote
  });

  document.getElementById("blood-type").replaceChildren(
    ...BLOOD_GROUPS.map((group) => h("option", { value: group }, group)));

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
    locationField.value = me.location || { area: me.area, latitude: me.latitude, longitude: me.longitude };
    phoneField.hidden = !me.is_donor;
    if (me.is_donor) {
      document.getElementById("phone").value = me.donor_profile.phone;
    }
  }

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

    const body = Object.assign({ name, blood_type: document.getElementById("blood-type").value }, locationField.value);
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
    locationNote.textContent = LOCATION_HINT;
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
