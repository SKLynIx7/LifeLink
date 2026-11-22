// Login, registration and password-reset pages.

const LifeLinkAuth = (() => {
  const { api, showMessage, saveSession, getSession, DASHBOARD, BLOOD_GROUPS, h } = LifeLink;

  const EMAIL_PATTERN = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

  function value(id) {
    return document.getElementById(id).value;
  }

  function setBusy(form, busy, label) {
    const button = form.querySelector("button[type=submit]");
    if (!button.dataset.label) {
      button.dataset.label = button.textContent;
    }
    button.disabled = busy;
    button.textContent = busy ? label : button.dataset.label;
  }

  async function logIn(email, password) {
    const result = await api("POST", "/api/login", { email, password });
    if (result.ok) {
      saveSession({ token: result.data.session_token, name: result.data.name });
      window.location.href = DASHBOARD;
    }
    return result;
  }

  function goToDashboardIfLoggedIn() {
    if (getSession()) {
      window.location.replace(DASHBOARD);
      return true;
    }
    return false;
  }

  // ---------------- Login ----------------
  function initLogin() {
    if (goToDashboardIfLoggedIn()) return;

    const form = document.getElementById("login-form");
    const message = document.getElementById("login-message");
    const params = new URLSearchParams(window.location.search);
    if (params.has("expired")) {
      showMessage(message, "Your session has ended. Please log in again.", "info");
    } else if (params.has("reset")) {
      showMessage(message, "Password updated. Please log in with your new password.", "success");
    }

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const email = value("email").trim();
      const password = value("password");
      if (!EMAIL_PATTERN.test(email) || !password) {
        showMessage(message, "Enter your email and password.");
        return;
      }
      setBusy(form, true, "Logging in…");
      const result = await logIn(email, password);
      if (!result.ok) {
        setBusy(form, false);
        showMessage(message, result.data.error || "Login failed. Please try again.");
      }
    });
  }

  // ---------------- Register ----------------
  function initRegister() {
    if (goToDashboardIfLoggedIn()) return;

    const form = document.getElementById("register-form");
    const message = document.getElementById("register-message");
    // Map picker, with the Dehradun area list as a fallback that needs no internet.
    const locationField = LocationPicker.attachField({
      preview: document.getElementById("location-preview"),
      button: document.getElementById("choose-location"),
      select: document.getElementById("area"),
      note: document.getElementById("location-note")
    });

    document.getElementById("blood-type").replaceChildren(
      h("option", { value: "" }, "Choose…"),
      ...BLOOD_GROUPS.map((group) => h("option", { value: group }, group)));

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const body = {
        name: value("name").trim(),
        email: value("email").trim(),
        password: value("password"),
        security_question: value("security-question"),
        security_answer: value("security-answer").trim()
      };

      // Same checks as the server, so most mistakes are caught before sending.
      let problem = "";
      if (!body.name) problem = "Enter your name.";
      else if (!EMAIL_PATTERN.test(body.email)) problem = "Enter a valid email address.";
      else if (body.password.length < 8) problem = "Password must be at least 8 characters.";
      else if (body.password !== value("confirm-password")) problem = "The two passwords do not match.";
      else if (!body.security_answer) problem = "Enter an answer to your security question.";

      const bloodType = document.getElementById("blood-type").value;
      const location = locationField.value;
      if (!problem && !bloodType) problem = "Choose your blood group.";
      else if (!problem && !location) problem = "Choose your location on the map or pick your area.";
      if (!problem) {
        body.blood_type = bloodType;
        Object.assign(body, location);
      }
      if (problem) {
        showMessage(message, problem);
        return;
      }

      setBusy(form, true, "Creating account…");
      const result = await api("POST", "/api/register", body);
      if (!result.ok) {
        setBusy(form, false);
        showMessage(message, result.data.error || "Registration failed. Please try again.");
        return;
      }
      // Log straight in after registering.
      const login = await logIn(body.email, body.password);
      if (!login.ok) {
        window.location.href = "/login.html";
      }
    });
  }

  // ---------------- Forgot password ----------------
  function initForgot() {
    const message = document.getElementById("forgot-message");
    const emailStep = document.getElementById("email-step");
    const answerStep = document.getElementById("answer-step");
    let email = "";

    emailStep.addEventListener("submit", async (event) => {
      event.preventDefault();
      email = value("email").trim();
      if (!EMAIL_PATTERN.test(email)) {
        showMessage(message, "Enter a valid email address.");
        return;
      }
      setBusy(emailStep, true, "Checking…");
      const result = await api("POST", "/api/security-question", { email });
      setBusy(emailStep, false);
      if (!result.ok) {
        showMessage(message, result.data.error || "Could not find that account.");
        return;
      }
      showMessage(message, "");
      document.getElementById("question-text").textContent = result.data.security_question;
      emailStep.hidden = true;
      answerStep.hidden = false;
      document.getElementById("security-answer").focus();
    });

    answerStep.addEventListener("submit", async (event) => {
      event.preventDefault();
      const answer = value("security-answer").trim();
      const newPassword = value("new-password");
      if (!answer) {
        showMessage(message, "Enter your answer.");
        return;
      }
      if (newPassword.length < 8) {
        showMessage(message, "New password must be at least 8 characters.");
        return;
      }
      if (newPassword !== value("confirm-password")) {
        showMessage(message, "The two passwords do not match.");
        return;
      }
      setBusy(answerStep, true, "Saving…");
      const result = await api("POST", "/api/forgot-password",
        { email, security_answer: answer, new_password: newPassword });
      if (!result.ok) {
        setBusy(answerStep, false);
        showMessage(message, result.data.error || "Could not reset the password.");
        return;
      }
      window.location.href = "/login.html?reset=1";
    });

    document.getElementById("change-email").addEventListener("click", () => {
      answerStep.hidden = true;
      emailStep.hidden = false;
      showMessage(message, "");
    });
  }

  return { initLogin, initRegister, initForgot };
})();
