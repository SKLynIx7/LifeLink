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
  // Search radius: "auto" widens 5 -> 10 -> 15 -> 20 -> 25 km; the server never goes beyond 25 km.
  const RADIUS_STEPS = [5, 10, 15, 20, 25];
  const MAX_RADIUS = 25;
  const STEP_PAUSE_MS = 450;  // long enough to read each "expanding" message
  const requestMessage = document.getElementById("request-message");
  const listMessage = document.getElementById("list-message");
  const list = document.getElementById("request-list");

  const state = {
    requests: [],
    expandedId: null,
    candidates: {},          // request id -> { candidates, invitations_left, radius_km, auto_expand, radii_tried }
    radiusMode: {},          // request id -> "auto" or 5 / 10 / 15 / 20 / 25
    searchStatus: {},        // request id -> message shown while the search is widening
    searching: new Set(),    // request ids with a search in progress
    selected: new Set(),     // donor ids ticked in the expanded request
    lastRendered: ""
  };

  // ---------------- New request form ----------------
  // Hospitals as cards (hospital-list.js), nearest to the user's saved location first.
  let hospitalList = null;
  async function loadHospitals() {
    const me = await api("GET", "/api/me");
    const origin = me.ok && me.data.location
      ? { latitude: me.data.location.latitude, longitude: me.data.location.longitude } : null;
    hospitalList = HospitalList.create(document.getElementById("hospital-list"), {
      mode: "select",
      origin,
      onSelect: () => showMessage(requestMessage, "")
    });
  }

  document.getElementById("blood-group").replaceChildren(
    ...BLOOD_GROUPS.map((group) => h("option", { value: group }, group))
  );
  function radiusOptions(selected) {
    return [
      h("option", { value: "auto", selected: selected === "auto" }, "Auto-expand (5 → 25 km)"),
      ...RADIUS_STEPS.map((km) => h("option", { value: String(km), selected: selected === km }, `${km} km`))
    ];
  }
  const radiusSelect = document.getElementById("radius");
  radiusSelect.replaceChildren(...radiusOptions("auto"));

  function readRadius(value) {
    return value === "auto" ? "auto" : Number(value);
  }

  document.getElementById("units").replaceChildren(
    ...Array.from({ length: 10 }, (_, index) => h("option", { value: String(index + 1) }, String(index + 1)))
  );

  document.getElementById("request-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const button = event.target.querySelector("button[type=submit]");
    const hospital = hospitalList ? hospitalList.selected : null;
    if (!hospital) {
      showMessage(requestMessage, "Choose a hospital that is open now.");
      document.getElementById("hospital-list").scrollIntoView({ behavior: "smooth", block: "center" });
      return;
    }
    const mode = readRadius(radiusSelect.value);
    button.disabled = true;
    const result = await api("POST", "/api/requests", {
      hospital_id: hospital.id,
      blood_type: document.getElementById("blood-group").value,
      units_required: Number(document.getElementById("units").value),
      radius_km: mode
    });
    button.disabled = false;
    if (!result.ok) {
      showMessage(requestMessage, result.data.error || "Could not create the request.");
      // The server re-checks the hospital: if it has just closed, refresh the cards.
      if (result.status === 409 && result.data.hospital_status) hospitalList.showUnavailableMessage(result.data.error);
      return;
    }
    const requestId = result.data.request_id;
    showMessage(requestMessage, "Request created. Choose whom to invite below.", "success");
    state.expandedId = requestId;
    state.selected.clear();
    state.radiusMode[requestId] = mode;
    state.searching.add(requestId);  // the step-by-step search below loads the donors
    await refresh(true);
    list.scrollIntoView({ behavior: "smooth", block: "start" });
    await searchDonors(requestId, { showSteps: true });
  });

  // ---------------- Data loading ----------------
  function radiusModeOf(requestId) {
    return state.radiusMode[requestId] ?? "auto";
  }

  async function fetchCandidates(requestId, radius) {
    const result = await api("GET", `/api/requests/${requestId}/candidates?radius_km=${radius}`);
    return result.ok ? result.data : null;
  }

  function storeCandidates(requestId, data) {
    if (!data) {
      delete state.candidates[requestId];
      return;
    }
    state.candidates[requestId] = data;
    // Drop ticks for donors who can no longer be invited (or are now outside the radius).
    const available = new Set(data.candidates.map((donor) => donor.id));
    [...state.selected].forEach((id) => { if (!available.has(id)) state.selected.delete(id); });
  }

  // Quiet reload for the 10-second refresh: one call, the server does any widening.
  async function loadCandidates(requestId) {
    if (state.searching.has(requestId)) return;
    storeCandidates(requestId, await fetchCandidates(requestId, radiusModeOf(requestId)));
  }

  const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

  // Search shown step by step: with auto-expand, tries 5, 10, 15, 20 and 25 km in turn
  // and tells the receiver each time it widens. A fixed radius is a single search.
  async function searchDonors(requestId, { showSteps } = {}) {
    const mode = radiusModeOf(requestId);
    state.searching.add(requestId);
    state.selected.clear();
    delete state.candidates[requestId];
    try {
      if (mode !== "auto" || !showSteps) {
        state.searchStatus[requestId] = mode === "auto" ? "Searching nearby donors…" : `Searching within ${mode} km…`;
        render();
        storeCandidates(requestId, await fetchCandidates(requestId, mode));
        return;
      }
      state.searchStatus[requestId] = "Searching nearby donors…";
      render();
      for (const [index, km] of RADIUS_STEPS.entries()) {
        const data = await fetchCandidates(requestId, km);
        if (!data) {
          storeCandidates(requestId, null);
          return;
        }
        const last = index === RADIUS_STEPS.length - 1;
        if (data.candidates.length || last) {
          storeCandidates(requestId, { ...data, auto_expand: true, radii_tried: RADIUS_STEPS.slice(0, index + 1) });
          return;
        }
        state.searchStatus[requestId] =
          `No donors within ${km} km. Expanding search to ${RADIUS_STEPS[index + 1]} km…`;
        render();
        await pause(STEP_PAUSE_MS);
      }
    } finally {
      state.searching.delete(requestId);
      delete state.searchStatus[requestId];
      state.lastRendered = "";
      render();
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
    if (expanded && expanded.status === "open" && !state.searching.has(expanded.request_id)) {
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
    // Send the radius on screen, so the server checks the same list the receiver sees.
    const shown = state.candidates[request.request_id];
    const result = await api("POST", `/api/requests/${request.request_id}/invite`,
      { ...body, radius_km: shown ? shown.radius_km : radiusModeOf(request.request_id) });
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

  function renderRadiusControl(request) {
    const requestId = request.request_id;
    const select = h("select", {
      "aria-label": "Search radius",
      disabled: state.searching.has(requestId),
      onchange: (event) => {
        state.radiusMode[requestId] = readRadius(event.target.value);
        searchDonors(requestId, { showSteps: true });
      }
    }, ...radiusOptions(radiusModeOf(requestId)));
    return h("label", { class: "radius-control" }, "Search radius", select);
  }

  function radiusSummary(info) {
    const count = info.candidates.length;
    const found = `${count} donor${count === 1 ? "" : "s"} found`;
    const widened = info.auto_expand && info.radii_tried && info.radii_tried.length > 1
      ? ` (widened from ${info.radii_tried[0]} km)` : "";
    return h("p", { class: "radius-summary", role: "status" },
      h("strong", {}, `Showing donors within ${info.radius_km} km of the hospital`), `${widened} · ${found}`);
  }

  function emptyCandidates(info) {
    if (info.auto_expand || info.radius_km >= MAX_RADIUS) {
      return `No available donors found within ${MAX_RADIUS} km. Try changing the blood group or hospital, ` +
        "or check again later. Invitations that expire or are declined free up their slots.";
    }
    return `No available donors within ${info.radius_km} km. Choose a larger radius or Auto-expand.`;
  }

  function renderCandidates(request) {
    const info = state.candidates[request.request_id];
    const status = state.searchStatus[request.request_id];
    const controls = h("div", { class: "candidate-header" },
      h("p", { class: "section-label" }, "Choose donors to invite"),
      renderRadiusControl(request));

    if (status || !info) {
      return h("div", { class: "panel" }, controls,
        h("p", { class: "search-status", role: "status", "aria-live": "polite" },
          h("span", { class: "spinner", "aria-hidden": "true" }), status || "Searching nearby donors…"));
    }
    const slots = info.invitations_left;
    const donors = info.candidates;

    const header = h("div", {},
      controls,
      radiusSummary(info),
      h("p", { class: "hint slots" }, `${slots} of ${MAX_OPEN_INVITATIONS} invitation slots free`));

    if (!donors.length) {
      return h("div", { class: "panel" }, header,
        h("p", { class: "empty-state" }, emptyCandidates(info)));
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
