# LifeLink

Emergency blood donor matching system. A receiver picks a hospital, blood group and number of
units; LifeLink finds compatible, eligible donors within 20 km and contacts them one at a time in
priority order.

## Tech stack

C++17, Crow (web server), SQLite, nlohmann/json, Windows BCrypt (PBKDF2 password hashing),
CMake + vcpkg, plain HTML/CSS/JavaScript frontend.

> **Windows only.** Password hashing uses the Windows BCrypt API, so the project builds on
> Windows with Visual Studio (MSVC).

## Build

```powershell
vcpkg install crow nlohmann-json sqlite3
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

The build copies the `web/` folder next to the executable.

## Run

```powershell
build\Release\LifeLink.exe                    # http://127.0.0.1:8080
build\Release\LifeLink.exe --port 9000 --db data.db --web-dir web
build\Release\LifeLink.exe --engine-demo      # ranking demo, no server
build\Release\LifeLink.exe --database-demo    # SQLite demo, in-memory database
```

If you see "created by an older version of LifeLink", delete `lifelink.db`; it is recreated
automatically with the new schema.

## Web pages (`web/`)

| Page | What it does |
|---|---|
| `login.html` | The first screen. `index.html` sends visitors here, or to the dashboard if already logged in |
| `register.html` | Create an account: name, email, password, **blood group, area in Dehradun** (or current location), security question |
| `forgot.html` | Reset the password with the security question |
| `dashboard.html` | Two choices after login: **Receive** and **Donate**, each with a one-line status |
| `receiver.html` | Request blood, choose and invite donors, see who accepted (with phone), confirm the donation, rate the donor |
| `profile.html` | Change name, blood group, area, mobile number (donors) and password. Opened from the menu under your name |
| `donor.html` | First time: mobile number and last donation date (blood group and area come from registration). After that: only the requests for you, or "No requests right now" |

Every page after login has a **bell** at the top right. A red dot means a request is waiting for
your reply (as a donor) or a donor has accepted your request (as a receiver). The menu under your
name shows your donor details, the availability switch, and Log out.

`app.js` holds shared helpers (API calls, session, safe HTML building, the header);
`auth.js`, `dashboard.js`, `receiver.js` and `donor.js` belong to their pages. Pages refresh every
10 seconds, so new requests and replies appear without reloading.

## Matching rules

- **Compatibility:** `unordered_map` from receiver blood group to allowed donor groups (`engine/BloodType.h`).
- **Eligibility:** donor is available and last donated at least 90 days ago.
- **Search radius:** 20 km from the hospital (haversine distance).
- **Hospitals:** six Dehradun hospitals are added automatically (`Database::seedHospitals`); their map positions are approximate.
- **Ranking:** `priority_queue` ordered by
  `0.65 × (1 − distance/20) + 0.35 × (rating/5)²`. The receiver sees donors in this order;
  the top 3 are marked "recommended", and "invite top N" pops the best N off the heap.

## Request flow

1. The receiver creates a request and gets the ranked list of compatible donors (never themself). Nobody is contacted yet.
2. The receiver invites the donors they choose (or the top N). At most **10 invitations** can be
   waiting at once, and each one **expires after 1 hour**.
3. An invited donor answers the **eligibility questionnaire** (`engine/Eligibility.h`, based on the
   NBTC donor selection criteria) and accepts, or declines. Answers are checked and not stored.
4. **As soon as a donor accepts, every other waiting invitation for that request is cancelled**, and
   that donor's invitations from other receivers are cancelled too. The receiver now sees the
   donor's phone number. If more units are still needed, the receiver invites again.
5. After the donation, the receiver marks the request **completed**. This records the donation
   date (restarting the 90-day rule) and lets the receiver **rate** the donor. Every donor starts
   at 5.0, and that counts as one rating: score = (5 + sum of stars) ÷ (1 + number of ratings).

| Status | Values |
|---|---|
| Request | `open` → `fulfilled` → `completed`, or `cancelled` |
| Invitation | `pending` → `accepted` / `rejected` / `cancelled` / `expired` |

## API

Authenticated routes need the header `X-Session-Token: <token>`.

| Method | Route | Who | Purpose |
|---|---|---|---|
| GET | `/ping` | anyone | Health check |
| POST | `/api/register` | anyone | Create an account with blood group and location (one account can receive and donate) |
| POST | `/api/login` | anyone | Returns a session token (valid 24 h) |
| POST | `/api/logout` | logged in | Ends the session |
| GET | `/api/me` | logged in | Current user, `is_donor`, and donor details |
| POST | `/api/donor/profile` | logged in | Become a donor: phone and last donation date |
| GET | `/api/notifications` | logged in | The bell: waiting requests (as donor) and acceptances (as receiver) |
| POST | `/api/profile` | logged in | Change name, blood group, area/location, and phone (donors) |
| POST | `/api/change-password` | logged in | `{"current_password","new_password"}`; other devices are logged out |
| POST | `/api/notifications/seen` | logged in | Marks acceptances as seen (clears the receiver's red dot) |
| POST | `/api/security-question` | anyone | Security question for an email |
| POST | `/api/forgot-password` | anyone | Reset password with the security answer |
| GET | `/api/hospitals` | anyone | Registered hospitals |
| GET | `/api/eligibility-questions` | anyone | The pre-donation questionnaire |
| POST | `/api/requests` | logged in | Create a request; returns ranked donors |
| GET | `/api/my-requests` | logged in | All my requests with their invitations |
| GET | `/api/requests/{id}` | logged in | One request with its invitations |
| GET | `/api/requests/{id}/candidates` | logged in | Donors that can still be invited |
| POST | `/api/requests/{id}/invite` | logged in | `{"donor_ids":[..]}` or `{"top":N}` |
| POST | `/api/requests/{id}/complete` | logged in | Confirm the donation happened |
| POST | `/api/requests/{id}/cancel` | logged in | Cancel the request |
| POST | `/api/ratings` | logged in | `{"request_id","donor_id","stars"}` after completion |
| GET | `/api/donor/invitations` | donor profile | My invitations (last 30 days) |
| POST | `/api/donor/availability` | donor profile | `{"available":true/false}`; turning off also withdraws waiting invitations |
| POST | `/api/invitations/{id}/respond` | donor profile | `{"accepted":false}` or `{"accepted":true,"answers":{..}}` |

Five failed login or reset attempts lock the email for 15 minutes.
