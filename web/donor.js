// Donate page: one-time donor setup, then only the requests for this donor,
// with the eligibility questionnaire before accepting.

(() => {
  const { api, h, showMessage, formatTime, minutesLeft } = LifeLink;

  const session = LifeLink.requireLogin();
  if (!session) {
    return;
  }
  LifeLink.setupHeader(session, { back: true });

  const PHONE_PATTERN = /^[6-9][0-9]{9}$/;

  const setupSection = document.getElementById("setup");
  const requestsSection = document.getElementById("requests");
  const list = document.getElementById("invitation-list");
  const inviteMessage = document.getElementById("invite-message");

  const modal = document.getElementById("questionnaire");
  const questionForm = document.getElementById("questionnaire-form");
  const questionList = document.getElementById("question-list");
  const questionMessage = document.getElementById("questionnaire-message");
  const reasonsList = document.getElementById("ineligible-reasons");
  const declineAfterCheck = document.getElementById("decline-after-check");

  const state = { me: null, invitations: [], questions: [], answering: null, lastRendered: "", started: false };

  // ---------------- One-time donor setup ----------------
  function initSetup() {
    const form = document.getElementById("setup-form");
    const message = document.getElementById("setup-message");
    document.getElementById("last-donation").max = new Date().toISOString().slice(0, 10);

    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const phone = document.getElementById("phone").value.trim();
      const lastDonation = document.getElementById("last-donation").value;
      if (!PHONE_PATTERN.test(phone)) {
        showMessage(message, "Enter a valid 10-digit mobile number.");
        return;
      }

      const button = form.querySelector("button[type=submit]");
      button.disabled = true;
      const body = { phone };
      if (lastDonation) body.last_donation_date = lastDonation;
      const result = await api("POST", "/api/donor/profile", body);
      button.disabled = false;
      if (!result.ok) {
        showMessage(message, result.data.error || "Could not save your details.");
        return;
      }
      LifeLink.announceChange();
      await start();
      showMessage(inviteMessage, "You are now registered as a donor. Requests for your blood group will appear here.", "success");
    });
  }

  // ---------------- Requests ----------------
  async function start() {
    const me = await api("GET", "/api/me");
    if (!me.ok) return;
    state.me = me.data;
    if (!state.me.is_donor) {
      document.getElementById("setup-account").replaceChildren(
        h("span", { class: "hint" }, "From your account"),
        h("strong", {}, `Blood group ${state.me.blood_type} · ${state.me.area}`));
      setupSection.hidden = false;
      requestsSection.hidden = true;
      return;
    }
    setupSection.hidden = true;
    requestsSection.hidden = false;
    if (!state.started) {
      loadQuestions();
    }
    await refresh(true);
    if (!state.started) {
      state.started = true;
      LifeLink.startPolling(() => refresh(false), 10);
      document.addEventListener("lifelink:changed", () => refresh(true));
    }
  }

  async function refresh(force) {
    if (state.answering || !state.me || !state.me.is_donor) {
      return;  // do not redraw while the donor is filling in the questionnaire
    }
    const [me, invitations] = await Promise.all([api("GET", "/api/me"), api("GET", "/api/donor/invitations")]);
    if (me.ok) state.me = me.data;
    if (!invitations.ok) {
      showMessage(inviteMessage, invitations.data.error || "Could not load your requests.");
      return;
    }
    state.invitations = invitations.data.invitations;
    const snapshot = JSON.stringify([state.me.donor_profile, state.invitations]);
    if (force || snapshot !== state.lastRendered) {
      state.lastRendered = snapshot;
      render();
    }
  }

  function render() {
    const pending = state.invitations.filter((invitation) => invitation.status === "pending");
    const agreed = state.invitations.filter((invitation) =>
      invitation.status === "accepted" && ["open", "fulfilled"].includes(invitation.request_status));
    const profile = state.me.donor_profile;

    const sections = agreed.map((invitation) => h("div", { class: "panel success-panel" },
      h("p", { class: "section-label" }, "You agreed to donate"),
      h("strong", {}, invitation.hospital_name),
      h("p", { class: "muted small-text" },
        `${invitation.hospital_address} · for ${invitation.receiver_name} · ${invitation.blood_type}`),
      h("p", { class: "hint" }, "Please go to the hospital as soon as you can. The person who asked has your phone number.")));

    pending.forEach((invitation) => sections.push(renderPending(invitation, agreed.length > 0)));

    if (!sections.length) {
      sections.push(h("div", { class: "empty-state" },
        h("span", { class: "empty-icon", "aria-hidden": "true" }, "♥"),
        h("strong", {}, "No requests right now"),
        h("p", {}, profile && !profile.available
          ? "You are marked as not available. Switch it on from the menu under your name to receive requests."
          : profile && profile.can_donate_again_in_days > 0
            ? `You can donate again in ${profile.can_donate_again_in_days} days. Requests will start appearing after that.`
            : "When someone near you needs your blood group, their request will appear here.")));
    }
    list.replaceChildren(...sections);
  }

  function renderPending(invitation, alreadyCommitted) {
    return h("div", { class: "donor-row invitation" },
      h("div", { class: "donor-info" },
        h("strong", {}, `${invitation.receiver_name} needs ${invitation.blood_type} blood`),
        h("p", {}, `${invitation.hospital_name}, ${invitation.hospital_address}`),
        h("p", {}, `${invitation.units_required} unit(s) · ${formatTime(invitation.invited_at)} · `,
          h("strong", {}, `${minutesLeft(invitation.expires_at)} min left to reply`))),
      h("div", { class: "action-column" },
        h("button", {
          type: "button",
          disabled: alreadyCommitted,
          title: alreadyCommitted ? "Finish your current donation first" : null,
          onclick: () => openQuestionnaire(invitation)
        }, "Accept"),
        h("button", { type: "button", class: "secondary", onclick: () => decline(invitation) }, "Decline")));
  }

  async function decline(invitation) {
    const result = await api("POST", `/api/invitations/${invitation.invitation_id}/respond`, { accepted: false });
    closeQuestionnaire();
    await refresh(true);
    LifeLink.announceChange();
    showMessage(inviteMessage, result.ok ? result.data.message : (result.data.error || "Could not send your reply."),
      result.ok ? "info" : "error");
  }

  // ---------------- Eligibility questionnaire ----------------
  async function loadQuestions() {
    const result = await api("GET", "/api/eligibility-questions");
    if (result.ok) {
      state.questions = result.data.questions;
      document.getElementById("questionnaire-note").textContent = result.data.note;
    }
  }

  async function openQuestionnaire(invitation) {
    if (!state.questions.length) {
      await loadQuestions();  // the donor may tap Accept before the questions have arrived
    }
    if (!state.questions.length) {
      showMessage(inviteMessage, "Could not load the health questions. Please refresh the page.");
      return;
    }
    state.answering = invitation;
    showMessage(questionMessage, "");
    reasonsList.hidden = true;
    declineAfterCheck.hidden = true;
    questionList.replaceChildren(...state.questions.map((question) => h("li", {},
      h("p", {}, question.text),
      h("span", { class: "yes-no" },
        h("label", { class: "choice" }, h("input", { type: "radio", name: question.id, value: "yes" }), " Yes"),
        h("label", { class: "choice" }, h("input", { type: "radio", name: question.id, value: "no" }), " No")))));
    modal.hidden = false;
    document.body.classList.add("modal-open");
    questionList.querySelector("input").focus();
  }

  function closeQuestionnaire() {
    modal.hidden = true;
    document.body.classList.remove("modal-open");
    state.answering = null;
  }

  questionForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const answers = {};
    for (const question of state.questions) {
      const chosen = questionForm.querySelector(`input[name="${question.id}"]:checked`);
      if (!chosen) {
        showMessage(questionMessage, "Please answer every question.");
        return;
      }
      answers[question.id] = chosen.value === "yes";
    }

    const invitation = state.answering;
    const button = questionForm.querySelector("button[type=submit]");
    button.disabled = true;
    const result = await api("POST", `/api/invitations/${invitation.invitation_id}/respond`,
      { accepted: true, answers });
    button.disabled = false;

    if (result.ok) {
      closeQuestionnaire();
      await refresh(true);
      LifeLink.announceChange();
      showMessage(inviteMessage, `${result.data.message} Hospital: ${result.data.hospital_name}, ${result.data.hospital_address}.`, "success");
      return;
    }
    if (result.data.reasons) {
      // Not eligible today: explain why and offer to decline.
      showMessage(questionMessage, result.data.error);
      reasonsList.replaceChildren(...result.data.reasons.map((reason) => h("li", {}, reason)));
      reasonsList.hidden = false;
      declineAfterCheck.hidden = false;
      modal.querySelector(".modal").scrollTop = 0;
      return;
    }
    // Expired, taken by someone else, etc.
    closeQuestionnaire();
    await refresh(true);
    showMessage(inviteMessage, result.data.error || "Could not accept this request.");
  });

  document.getElementById("close-questionnaire").addEventListener("click", closeQuestionnaire);
  declineAfterCheck.addEventListener("click", () => decline(state.answering));
  modal.addEventListener("click", (event) => { if (event.target === modal) closeQuestionnaire(); });
  document.addEventListener("keydown", (event) => { if (event.key === "Escape" && !modal.hidden) closeQuestionnaire(); });

  initSetup();
  start();
})();
