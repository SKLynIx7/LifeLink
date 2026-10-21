// Receiver dashboard: create a request, choose donors to invite, follow replies,
// confirm the donation and rate the donor.

(() => {
  const { api, h, showMessage, formatTime, minutesLeft, statusBadge, BLOOD_GROUPS } = LifeLink;

  const session = LifeLink.requireLogin();
  if (!session) {
    return;
  }
  LifeLink.setupHeader(session, { back: true });

  const MAX_OPEN_INVITATIONS = 10;
  const requestMessage = document.getElementById("request-message");
  const listMessage = document.getElementById("list-message");
  const list = document.getElementById("request-list");

  const state = {
    requests: [],
    expandedId: null,
    candidates: {},          // request id -> { candidates, invitations_left }
    selected: new Set(),     // donor ids ticked in the expanded request
    lastRendered: ""
  };

  // ---------------- New request form ----------------
  async function loadHospitals() {
    const result = await api("GET", "/api/hospitals");
    const select = document.getElementById("hospital");
    if (!result.ok) {
      showMessage(requestMessage, "Could not load hospitals. Please refresh the page.");
      return;
    }
    select.replaceChildren(
      ...result.data.hospitals.map((hospital) =>
        h("option", { value: String(hospital.id) }, `${hospital.name} — ${hospital.address}`))
    );
  }

  document.getElementById("blood-group").replaceChildren(
    ...BLOOD_GROUPS.map((group) => h("option", { value: group }, group))
  );
  document.getElementById("units").replaceChildren(
    ...Array.from({ length: 10 }, (_, index) => h("option", { value: String(index + 1) }, String(index + 1)))
  );

  document.getElementById("request-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const button = event.target.querySelector("button");
    button.disabled = true;
    const result = await api("POST", "/api/requests", {
      hospital_id: Number(document.getElementById("hospital").value),
      blood_type: document.getElementById("blood-group").value,
      units_required: Number(document.getElementById("units").value)
    });
    button.disabled = false;
    if (!result.ok) {
      showMessage(requestMessage, result.data.error || "Could not create the request.");
      return;
    }
    showMessage(requestMessage, result.data.candidates.length
      ? `Request created. ${result.data.candidates.length} compatible donors found — choose whom to invite below.`
      : "Request created, but no compatible donors are available within 20 km right now.",
      result.data.candidates.length ? "success" : "info");
    state.expandedId = result.data.request_id;
    state.selected.clear();
    state.candidates[result.data.request_id] = {
      candidates: result.data.candidates,
      invitations_left: MAX_OPEN_INVITATIONS
    };
    await refresh(true);
    list.scrollIntoView({ behavior: "smooth", block: "start" });
  });

  // ---------------- Data loading ----------------
  async function loadCandidates(requestId) {
    const result = await api("GET", `/api/requests/${requestId}/candidates`);
    if (result.ok) {
      state.candidates[requestId] = result.data;
      // Drop ticks for donors who can no longer be invited.
      const available = new Set(result.data.candidates.map((donor) => donor.id));
      [...state.selected].forEach((id) => { if (!available.has(id)) state.selected.delete(id); });
    } else {
      delete state.candidates[requestId];
    }
  }

  async function refresh(force) {
    const result = await api("GET", "/api/my-requests");
    if (!result.ok) {
      showMessage(listMessage, result.data.error || "Could not load your requests.");
      return;
    }
    state.requests = result.data.requests;

    // Open the newest active request by default.
    if (state.expandedId === null) {
      const active = state.requests.find((request) => ["open", "fulfilled"].includes(request.status));
      state.expandedId = active ? active.request_id : null;
    }
    const expanded = state.requests.find((request) => request.request_id === state.expandedId);
    if (expanded && expanded.status === "open") {
      await loadCandidates(expanded.request_id);
    }

    // Skip re-drawing when nothing changed, so the page does not jump while you read it.
    const snapshot = JSON.stringify([state.requests, state.candidates[state.expandedId], state.expandedId]);
    if (force || snapshot !== state.lastRendered) {
      state.lastRendered = snapshot;
      render();
    }
  }

  // ---------------- Actions ----------------
  async function invite(request, body) {
    showMessage(requestMessage, "");
    const result = await api("POST", `/api/requests/${request.request_id}/invite`, body);
    if (result.ok) {
      state.selected.clear();
    }
    await refresh(true);
    if (!result.ok) {
      showMessage(listMessage, result.data.error || "Could not send the invitations.");
    } else {
      const names = result.data.invited.map((donor) => donor.donor_name).join(", ");
      showMessage(listMessage, `Invitation sent to ${names}. Each invitation stays open for 1 hour.`, "success");
    }
  }

  async function complete(request) {
    if (!window.confirm("Confirm that the donation has taken place?")) return;
    const result = await api("POST", `/api/requests/${request.request_id}/complete`);
    await refresh(true);
    showMessage(listMessage, result.ok ? result.data.message : (result.data.error || "Could not update the request."),
      result.ok ? "success" : "error");
  }

  async function cancel(request) {
    if (!window.confirm("Cancel this request? Donors who have not answered will be told it is closed.")) return;
    const result = await api("POST", `/api/requests/${request.request_id}/cancel`);
    await refresh(true);
    showMessage(listMessage, result.ok ? "Request cancelled." : (result.data.error || "Could not cancel."),
      result.ok ? "info" : "error");
  }

  async function rate(request, invitation, stars) {
    const result = await api("POST", "/api/ratings",
      { request_id: request.request_id, donor_id: invitation.donor_id, stars });
    await refresh(true);
    showMessage(listMessage, result.ok ? `Thank you — you rated ${invitation.donor_name} ${stars} out of 5.`
      : (result.data.error || "Could not save the rating."), result.ok ? "success" : "error");
  }

  // ---------------- Rendering ----------------
  function render() {
    if (!state.requests.length) {
      list.replaceChildren(h("p", { class: "muted" }, "You have no requests yet. Create one above."));
      return;
    }
    list.replaceChildren(...state.requests.map(renderRequest));
  }

  function renderRequest(request) {
    const expanded = request.request_id === state.expandedId;
    const summary = h("button", {
      type: "button",
      class: "request-summary",
      "aria-expanded": expanded ? "true" : "false",
      onclick: () => {
        state.expandedId = expanded ? null : request.request_id;
        state.selected.clear();
        refresh(true);
      }
    },
      h("span", { class: "blood-chip" }, request.blood_type),
      h("span", { class: "summary-text" },
        h("strong", {}, request.hospital_name),
        h("small", {}, `${request.units_accepted} of ${request.units_required} unit(s) covered · ${formatTime(request.created_at)}`)),
      statusBadge(request.status),
      h("span", { class: "chevron" }, expanded ? "▲" : "▼"));

    return h("article", { class: `request-item ${expanded ? "expanded" : ""}` },
      summary, expanded ? renderDetails(request) : null);
  }

  function renderDetails(request) {
    const accepted = request.invitations.filter((invitation) => invitation.status === "accepted");
    const others = request.invitations.filter((invitation) => invitation.status !== "accepted");
    const active = ["open", "fulfilled"].includes(request.status);

    return h("div", { class: "request-details" },
      accepted.length ? h("div", { class: "panel success-panel" },
        h("p", { class: "section-label" }, "Donors who accepted"),
        ...accepted.map((invitation) => renderAcceptedDonor(request, invitation))) : null,

      active ? h("div", { class: "action-row" },
        accepted.length ? h("button", { type: "button", onclick: () => complete(request) }, "Donation done") : null,
        h("button", { type: "button", class: "secondary", onclick: () => cancel(request) }, "Cancel request")) : null,

      request.status === "open" ? renderCandidates(request) : null,

      others.length ? h("div", { class: "panel" },
        h("p", { class: "section-label" }, "Invitations sent"),
        ...others.map((invitation) => h("div", { class: "invite-row" },
          h("span", {}, h("strong", {}, invitation.donor_name), ` (${invitation.blood_type})`),
          h("span", { class: "invite-meta" },
            invitation.status === "pending" ? `${minutesLeft(invitation.expires_at)} min left` : null,
            statusBadge(invitation.status))))) : null,

      request.status === "fulfilled"
        ? h("p", { class: "hint" }, "All units are covered. Press “Donation done” after the donation so you can rate your donor.")
        : null
    );
  }

  function renderAcceptedDonor(request, invitation) {
    let rating = null;
    if (request.status === "completed") {
      rating = invitation.rated
        ? h("span", { class: "hint" }, "Rated — thank you")
        : h("span", { class: "stars", role: "group", "aria-label": `Rate ${invitation.donor_name}` },
            "Rate: ",
            ...[1, 2, 3, 4, 5].map((stars) => h("button", {
              type: "button", class: "star", title: `${stars} star${stars > 1 ? "s" : ""}`,
              onclick: () => rate(request, invitation, stars)
            }, "★")));
    }
    return h("div", { class: "accepted-row" },
      h("div", {},
        h("strong", {}, invitation.donor_name), ` · ${invitation.blood_type}`,
        h("p", {}, "Phone: ", h("a", { href: `tel:${invitation.donor_phone}` }, invitation.donor_phone),
          ` · accepted ${formatTime(invitation.responded_at)}`)),
      rating);
  }

  function renderCandidates(request) {
    const info = state.candidates[request.request_id];
    if (!info) {
      return h("p", { class: "muted" }, "Loading donors…");
    }
    const slots = info.invitations_left;
    const donors = info.candidates;

    const header = h("div", { class: "candidate-header" },
      h("p", { class: "section-label" }, "Choose donors to invite"),
      h("span", { class: "hint" }, `${slots} of ${MAX_OPEN_INVITATIONS} invitation slots free`));

    if (!donors.length) {
      return h("div", { class: "panel" }, header,
        h("p", { class: "muted" }, "No more compatible donors are available within 20 km right now. " +
          "Invitations that expire or are declined free up their slots."));
    }

    const rows = donors.map((donor) => {
      const checkbox = h("input", {
        type: "checkbox",
        checked: state.selected.has(donor.id),
        disabled: !state.selected.has(donor.id) && state.selected.size >= slots,
        onchange: (event) => {
          if (event.target.checked) state.selected.add(donor.id); else state.selected.delete(donor.id);
          render();
        }
      });
      return h("label", { class: `donor-row selectable ${donor.recommended ? "recommended" : ""}` },
        checkbox,
        h("div", { class: "donor-info" },
          h("strong", {}, `#${donor.rank} ${donor.name}`),
          h("p", {}, `${donor.blood_type} · ${donor.distance_km < 0.1 ? "less than 0.1" : donor.distance_km} km away · ` +
            `rating ${donor.reliability_score.toFixed(1)}/5`)),
        h("span", { class: "tags" },
          donor.recommended ? h("span", { class: "tag tag-good" }, "Recommended") : null,
          donor.low_reliability ? h("span", { class: "tag tag-warn" }, "Low reliability") : null));
    });

    const topCount = Math.min(3, slots, donors.length);
    return h("div", { class: "panel" }, header,
      h("p", { class: "hint" }, "Ranked by distance (65%) and reliability (35%). " +
        "The first donor to accept closes all other invitations."),
      ...rows,
      h("div", { class: "action-row" },
        h("button", {
          type: "button",
          disabled: state.selected.size === 0 || slots === 0,
          onclick: () => invite(request, { donor_ids: [...state.selected] })
        }, `Invite selected (${state.selected.size})`),
        topCount > 0 ? h("button", {
          type: "button", class: "secondary",
          onclick: () => invite(request, { top: topCount })
        }, `Invite top ${topCount}`) : null));
  }

  loadHospitals();
  refresh(true);
  LifeLink.startPolling(() => refresh(false), 10);
})();
