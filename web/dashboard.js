// Dashboard: the two choices, Receive and Donate, each with a one-line status.

(() => {
  const { api } = LifeLink;

  const session = LifeLink.requireLogin();
  if (!session) {
    return;
  }
  LifeLink.setupHeader(session);
  document.getElementById("greeting").textContent = `Hello, ${session.name.split(" ")[0]}`;

  const receiveStatus = document.getElementById("receive-status");
  const donateStatus = document.getElementById("donate-status");

  function plural(count, word) {
    return `${count} ${word}${count === 1 ? "" : "s"}`;
  }

  async function refresh() {
    const [requests, me] = await Promise.all([api("GET", "/api/my-requests"), api("GET", "/api/me")]);

    if (requests.ok) {
      const active = requests.data.requests.filter((request) => ["open", "fulfilled"].includes(request.status));
      const found = active.filter((request) => request.units_accepted > 0).length;
      if (!active.length) {
        receiveStatus.textContent = "No active requests";
      } else if (active.length === 1) {
        receiveStatus.textContent = found ? "Donor found · open to see their phone number" : "1 active request · looking for donors";
      } else {
        receiveStatus.textContent = `${active.length} active requests · ${found} with a donor`;
      }
    }

    if (me.ok && !me.data.is_donor) {
      donateStatus.textContent = "Set up your donor details (takes a minute)";
      return;
    }
    const invitations = await api("GET", "/api/donor/invitations");
    if (invitations.ok) {
      const waiting = invitations.data.invitations.filter((invitation) => invitation.status === "pending").length;
      donateStatus.textContent = waiting ? `${plural(waiting, "request")} waiting for your reply` : "No requests right now";
      donateStatus.classList.toggle("attention", waiting > 0);
    }
  }

  refresh();
  LifeLink.startPolling(refresh, 10);
  document.addEventListener("lifelink:changed", refresh);
})();
