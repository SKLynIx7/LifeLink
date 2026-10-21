// Shared helpers used by every LifeLink page: API calls, the login session,
// safe HTML building, and the header with the notification bell and name menu.

const LifeLink = (() => {
  const SESSION_KEY = "lifelink.session";
  const BLOOD_GROUPS = ["A+", "A-", "B+", "B-", "AB+", "AB-", "O+", "O-"];
  // Areas of Dehradun (approximate centre of each). Registration can also use the browser's location.
  const AREAS = [
    { name: "Clock Tower / Paltan Bazaar", latitude: 30.3243, longitude: 78.0414 },
    { name: "Rajpur Road", latitude: 30.3456, longitude: 78.0617 },
    { name: "Dalanwala", latitude: 30.3170, longitude: 78.0560 },
    { name: "Ballupur", latitude: 30.3335, longitude: 78.0110 },
    { name: "Vasant Vihar", latitude: 30.3260, longitude: 78.0060 },
    { name: "Patel Nagar", latitude: 30.3050, longitude: 78.0200 },
    { name: "Prem Nagar", latitude: 30.3340, longitude: 77.9600 },
    { name: "Jakhan", latitude: 30.3620, longitude: 78.0700 },
    { name: "Sahastradhara Road", latitude: 30.3420, longitude: 78.0880 },
    { name: "Raipur", latitude: 30.3100, longitude: 78.0900 },
    { name: "Clement Town", latitude: 30.2690, longitude: 78.0090 },
    { name: "ISBT / Majra", latitude: 30.2900, longitude: 78.0000 },
    { name: "Doiwala", latitude: 30.1760, longitude: 78.1240 }
  ];
  const DASHBOARD = "/dashboard.html";

  // ---- Session (token + name), kept in this browser only.
  function getSession() {
    try {
      const saved = JSON.parse(localStorage.getItem(SESSION_KEY));
      return saved && saved.token ? saved : null;
    } catch (error) {
      return null;
    }
  }

  function saveSession(session) {
    try {
      localStorage.setItem(SESSION_KEY, JSON.stringify(session));
    } catch (error) {
      // Private mode or blocked storage: the user will just need to log in again.
    }
  }

  function clearSession() {
    try {
      localStorage.removeItem(SESSION_KEY);
    } catch (error) {
      // nothing to clear
    }
  }

  // ---- API calls. Every call returns { ok, status, data }.
  async function api(method, path, body) {
    const headers = { "Content-Type": "application/json" };
    const session = getSession();
    if (session) {
      headers["X-Session-Token"] = session.token;
    }

    let response;
    try {
      response = await fetch(path, {
        method,
        headers,
        body: body === undefined ? undefined : JSON.stringify(body)
      });
    } catch (error) {
      return { ok: false, status: 0, data: { error: "Cannot reach the LifeLink server. Is it running?" } };
    }

    let data = {};
    try {
      data = await response.json();
    } catch (error) {
      data = {};
    }

    // Session expired or logged out elsewhere: go back to the login page.
    if (response.status === 401 && session && !path.startsWith("/api/login")) {
      clearSession();
      window.location.href = "/login.html?expired=1";
    }
    return { ok: response.ok, status: response.status, data };
  }

  // Sends logged-out visitors to the login page.
  function requireLogin() {
    const session = getSession();
    if (!session) {
      window.location.replace("/login.html");
      return null;
    }
    return session;
  }

  async function logout() {
    await api("POST", "/api/logout");
    clearSession();
    window.location.href = "/login.html";
  }

  // ---- DOM helper. Text is always set with textContent, so data from the
  // server can never inject HTML or scripts into the page.
  function h(tag, attributes, ...children) {
    const element = document.createElement(tag);
    for (const [key, value] of Object.entries(attributes || {})) {
      if (value === undefined || value === null || value === false) {
        continue;
      }
      if (key === "class") {
        element.className = value;
      } else if (key.startsWith("on") && typeof value === "function") {
        element.addEventListener(key.slice(2), value);
      } else if (value === true) {
        element.setAttribute(key, "");
      } else {
        element.setAttribute(key, value);
      }
    }
    for (const child of children.flat()) {
      if (child === null || child === undefined || child === false) {
        continue;
      }
      element.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return element;
  }

  // ---- Messages shown at the top of a form or section.
  function showMessage(target, text, kind) {
    target.textContent = text;
    target.className = `message ${kind || "error"}`;
    target.hidden = !text;
  }

  // SQLite stores times in UTC as "YYYY-MM-DD HH:MM:SS".
  function parseServerTime(value) {
    return value ? new Date(value.replace(" ", "T") + "Z") : null;
  }

  function formatTime(value) {
    const date = parseServerTime(value);
    return date ? date.toLocaleString([], { dateStyle: "medium", timeStyle: "short" }) : "";
  }

  function timeAgo(value) {
    const date = parseServerTime(value);
    if (!date) return "";
    const minutes = Math.max(0, Math.round((Date.now() - date) / 60000));
    if (minutes < 1) return "just now";
    if (minutes < 60) return `${minutes} min ago`;
    const hours = Math.round(minutes / 60);
    return hours < 24 ? `${hours} h ago` : formatTime(value);
  }

  function minutesLeft(value) {
    const date = parseServerTime(value);
    return date ? Math.max(0, Math.ceil((date - Date.now()) / 60000)) : 0;
  }

  function statusBadge(status) {
    const labels = {
      open: "Looking for donors", fulfilled: "Donor found", completed: "Completed", cancelled: "Cancelled",
      pending: "Waiting for reply", accepted: "Accepted", rejected: "Declined", expired: "Expired"
    };
    return h("span", { class: `status status-${status}` }, labels[status] || status);
  }

  // Refreshes a page every few seconds while the tab is open, so new
  // invitations and replies appear without reloading.
  function startPolling(callback, seconds) {
    let timer = setInterval(callback, seconds * 1000);
    document.addEventListener("visibilitychange", () => {
      clearInterval(timer);
      if (!document.hidden) {
        callback();
        timer = setInterval(callback, seconds * 1000);
      }
    });
  }

  // Pages listen for this to reload their data after something changes in the header.
  function announceChange() {
    document.dispatchEvent(new CustomEvent("lifelink:changed"));
  }

  // ---------------- Header: back link, bell and name menu ----------------
  const BELL_ICON = '<svg viewBox="0 0 24 24" width="22" height="22" aria-hidden="true" focusable="false">' +
    '<path fill="currentColor" d="M12 22a2.5 2.5 0 0 0 2.45-2h-4.9A2.5 2.5 0 0 0 12 22Zm7-6V11a7 7 0 0 0-5.5-6.84V3.5a1.5 1.5 0 0 0-3 0v.66A7 7 0 0 0 5 11v5l-1.6 1.6A.8.8 0 0 0 4 19h16a.8.8 0 0 0 .6-1.4Z"/></svg>';

  const header = { me: null, notifications: null, openPanel: null };

  function closePanels() {
    header.openPanel = null;
    document.querySelectorAll(".menu-panel").forEach((panel) => { panel.hidden = true; });
    document.querySelectorAll("[aria-expanded]").forEach((button) => button.setAttribute("aria-expanded", "false"));
  }

  function togglePanel(name, button, panel, onOpen) {
    const opening = header.openPanel !== name;
    closePanels();
    if (opening) {
      header.openPanel = name;
      panel.hidden = false;
      button.setAttribute("aria-expanded", "true");
      if (onOpen) onOpen();
    }
  }

  function renderBell(bellButton, panel) {
    const data = header.notifications || { items: [], unread: 0 };
    bellButton.querySelector(".dot").hidden = data.unread === 0;
    bellButton.setAttribute("aria-label", data.unread
      ? `Notifications, ${data.unread} new` : "Notifications");

    const items = data.items.map((item) => {
      if (item.type === "invitation") {
        return h("a", { class: "notice unread", href: "/donor.html" },
          h("strong", {}, `${item.person} needs ${item.blood_type} blood`),
          h("span", {}, `${item.hospital_name} · ${minutesLeft(item.expires_at)} min left to reply`));
      }
      return h("a", { class: `notice ${item.unread ? "unread" : ""}`, href: "/receiver.html" },
        h("strong", {}, `${item.person} accepted your ${item.blood_type} request`),
        h("span", {}, `${item.hospital_name} · ${timeAgo(item.at)} · see their phone number`));
    });
    panel.replaceChildren(
      h("p", { class: "menu-title" }, "Notifications"),
      ...(items.length ? items : [h("p", { class: "menu-empty" }, "Nothing new. Requests for your help and replies to your requests appear here.")])
    );
  }

  function renderNameMenu(panel) {
    const me = header.me;
    const rows = [];
    if (me && me.donor_profile) {
      const profile = me.donor_profile;
      rows.push(
        h("p", { class: "menu-title" }, "Your donor details"),
        h("dl", { class: "details" },
          h("dt", {}, "Blood group"), h("dd", {}, profile.blood_type),
          h("dt", {}, "Area"), h("dd", {}, me.area),
          h("dt", {}, "Rating"),
          h("dd", {}, `${profile.reliability_score.toFixed(1)} / 5`),
          h("dt", {}, "Last donation"), h("dd", {}, profile.last_donation_date || "Never"),
          h("dt", {}, "Can donate again"),
          h("dd", {}, profile.can_donate_again_in_days === 0 ? "Now" : `In ${profile.can_donate_again_in_days} days`)),
        h("label", { class: "toggle" },
          h("input", { type: "checkbox", id: "availability-toggle", checked: profile.available,
            onchange: (event) => setAvailability(event.target) }),
          h("span", {}, profile.available ? "Available to donate" : "Not available right now")),
        h("p", { class: "menu-note" }, profile.available
          ? "People near you who need your blood group can invite you."
          : "You are hidden from new requests."));
    } else if (me) {
      rows.push(
        h("dl", { class: "details" },
          h("dt", {}, "Blood group"), h("dd", {}, me.blood_type),
          h("dt", {}, "Area"), h("dd", {}, me.area)),
        h("p", { class: "menu-note" }, "Not registered as a donor yet."),
        h("a", { class: "menu-link", href: "/donor.html" }, "Become a donor"));
    }
    rows.push(
      h("a", { class: "menu-link", href: "/profile.html" }, "Edit profile"),
      h("button", { type: "button", class: "menu-logout", onclick: logout }, "Log out"));
    panel.replaceChildren(...rows);
  }

  async function setAvailability(checkbox) {
    const available = checkbox.checked;
    const waiting = header.notifications && header.notifications.items.some((item) => item.type === "invitation");
    if (!available && waiting &&
        !window.confirm("Turning this off also withdraws you from the requests waiting for your reply. Continue?")) {
      checkbox.checked = true;
      return;
    }
    checkbox.disabled = true;
    await api("POST", "/api/donor/availability", { available });
    await refreshHeader();
    if (headerParts) renderNameMenu(headerParts.namePanel);  // the open menu shows the new setting
    announceChange();
  }

  let headerParts = null;

  async function refreshHeader() {
    const [me, notifications] = await Promise.all([api("GET", "/api/me"), api("GET", "/api/notifications")]);
    if (me.ok) header.me = me.data;
    if (notifications.ok) header.notifications = notifications.data;
    if (!headerParts) return;
    renderBell(headerParts.bellButton, headerParts.bellPanel);
    if (header.openPanel !== "name") renderNameMenu(headerParts.namePanel);
    if (header.me) headerParts.nameButton.firstChild.textContent = header.me.name.split(" ")[0];
  }

  // Builds the right side of the header. Pass { back: true } on inner pages.
  function setupHeader(session, options) {
    const slot = document.getElementById("user-slot");
    if (!slot) return;

    const bellButton = h("button", {
      type: "button", class: "icon-button bell", "aria-label": "Notifications", "aria-expanded": "false",
      "aria-haspopup": "true"
    });
    bellButton.innerHTML = BELL_ICON;  // fixed icon markup, no user data
    bellButton.append(h("span", { class: "dot", hidden: true }));
    const bellPanel = h("div", { class: "menu-panel bell-panel", hidden: true });

    const nameButton = h("button", {
      type: "button", class: "name-button", "aria-expanded": "false", "aria-haspopup": "true"
    }, session.name.split(" ")[0], h("span", { class: "caret", "aria-hidden": "true" }, "▾"));
    const namePanel = h("div", { class: "menu-panel name-panel", hidden: true });

    bellButton.addEventListener("click", (event) => {
      event.stopPropagation();
      togglePanel("bell", bellButton, bellPanel, async () => {
        // Opening the bell marks "donor accepted" updates as seen.
        await api("POST", "/api/notifications/seen");
        await refreshHeader();
      });
    });
    nameButton.addEventListener("click", (event) => {
      event.stopPropagation();
      togglePanel("name", nameButton, namePanel, () => renderNameMenu(namePanel));
    });
    bellPanel.addEventListener("click", (event) => event.stopPropagation());
    namePanel.addEventListener("click", (event) => event.stopPropagation());
    document.addEventListener("click", closePanels);
    document.addEventListener("keydown", (event) => { if (event.key === "Escape") closePanels(); });

    slot.replaceChildren(...[
      options && options.back ? h("a", { class: "back-link", href: DASHBOARD }, "← Dashboard") : null,
      h("div", { class: "menu-anchor" }, bellButton, bellPanel),
      h("div", { class: "menu-anchor" }, nameButton, namePanel)
    ].filter(Boolean));
    headerParts = { bellButton, bellPanel, nameButton, namePanel };
    refreshHeader();
    startPolling(refreshHeader, 10);
    document.addEventListener("lifelink:changed", refreshHeader);
  }

  return {
    BLOOD_GROUPS, AREAS, DASHBOARD, getSession, saveSession, clearSession, api, requireLogin, logout,
    h, showMessage, formatTime, minutesLeft, timeAgo, statusBadge, setupHeader, startPolling, announceChange,
    getMe: () => header.me, refreshHeader
  };
})();
