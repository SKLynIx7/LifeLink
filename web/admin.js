// Administrator page: approve or reject newly registered hospitals.
// Only accounts made administrators with "LifeLink --create-admin <email>" can use it;
// the server checks this on every call.

(() => {
  const { api, h, showMessage, formatTime } = LifeLink;

  const session = LifeLink.requireLogin();
  if (!session) {
    return;
  }
  LifeLink.setupHeader(session, { back: true });

  const list = document.getElementById("admin-list");
  const message = document.getElementById("admin-message");
  const tabs = [...document.querySelectorAll(".admin-tab")];
  let status = "pending";

  const DAYS_TEXT = (hospital) => hospital.open_24_hours
    ? `${hospital.working_days.join(", ")} · 24 hours`
    : `${hospital.working_days.join(", ")} · ${hospital.opening_time}–${hospital.closing_time}`;

  function detail(label, value) {
    return value ? h("div", { class: "hl-detail" }, h("dt", {}, label), h("dd", {}, value)) : null;
  }

  function card(hospital) {
    const note = h("textarea", { rows: "2", maxlength: "300", placeholder: "Reason (needed to reject), e.g. Licence number could not be verified" });
    const decide = async (decision) => {
      if (decision === "rejected" && note.value.trim().length < 5) {
        showMessage(message, "Write a short reason before rejecting, so the hospital knows what to fix.");
        note.focus();
        return;
      }
      const result = await api("PUT", `/api/admin/hospitals/${hospital.id}/verification`,
        { status: decision, note: note.value.trim() });
      showMessage(message, result.ok ? `${hospital.name}: ${result.data.message}` : (result.data.error || "Could not save."),
        result.ok ? "success" : "error");
      load();
    };
    const actions = [];
    if (hospital.verification_status !== "verified") {
      actions.push(h("button", { type: "button", onclick: () => decide("verified") }, "Approve"));
    }
    if (hospital.verification_status !== "rejected") {
      actions.push(h("button", { type: "button", class: "secondary", onclick: () => decide("rejected") }, "Reject"));
    }
    return h("article", { class: `admin-card ${hospital.verification_status}` },
      h("div", { class: "hl-head" },
        h("h3", {}, hospital.name),
        h("span", { class: `verify-badge ${hospital.verification_status}` }, hospital.verification_status)),
      h("dl", { class: "hl-details" },
        detail("Registered", hospital.registered_at ? formatTime(hospital.registered_at) : "Listed hospital"),
        detail("Email", hospital.email),
        detail("Phone", hospital.phone),
        detail("Type", hospital.hospital_type),
        detail("Licence", hospital.license_number || "Not given"),
        detail("Address", [hospital.address, hospital.city, hospital.state, hospital.pincode, hospital.country].filter(Boolean).join(", ")),
        detail("Map", h("a", { href: hospital.map_url, target: "_blank", rel: "noopener noreferrer" },
          `${hospital.latitude.toFixed(5)}, ${hospital.longitude.toFixed(5)} ↗`)),
        detail("Hours", DAYS_TEXT(hospital)),
        detail("Blood groups", hospital.blood_groups.join(", ")),
        detail("Website", hospital.website),
        detail("Reason given", hospital.verification_status === "rejected" ? hospital.verification_note : "")),
      actions.length ? h("div", { class: "admin-decision" }, note, h("div", { class: "action-row" }, ...actions)) : null);
  }

  async function load() {
    tabs.forEach((tab) => {
      const active = tab.dataset.status === status;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-selected", active ? "true" : "false");
    });
    const result = await api("GET", `/api/admin/hospitals?status=${status}`);
    if (!result.ok) {
      list.replaceChildren(h("p", { class: "empty-state" },
        result.status === 403 ? "This page is for administrators only." : (result.data.error || "Could not load hospitals.")));
      return;
    }
    tabs.forEach((tab) => { tab.querySelector(".count").textContent = `(${result.data.counts[tab.dataset.status]})`; });
    const hospitals = result.data.hospitals;
    list.replaceChildren(...(hospitals.length ? hospitals.map(card)
      : [h("p", { class: "hl-empty" }, status === "pending" ? "No hospitals are waiting for approval." : `No ${status} hospitals.`)]));
  }

  tabs.forEach((tab) => tab.addEventListener("click", () => {
    status = tab.dataset.status;
    showMessage(message, "");
    load();
  }));
  load();
})();
