# LifeLink

Emergency blood donor matching for Dehradun. A receiver picks a hospital, blood group and number
of units; LifeLink ranks compatible, eligible donors near the hospital (5 to 25 km), the receiver
invites the ones they choose, and the first donor to accept is matched. Hospitals have their own
accounts to show whether they can take donors right now and to post urgent blood needs, and an
administrator approves new hospitals.

## Run locally

Open **PowerShell** and paste one of these blocks. Each one stops a running LifeLink, builds the
latest code, starts the server in its own window and opens the website at
**http://127.0.0.1:8080**. Keep the server window open while using the site; close it (or press
`Ctrl + C` in it) to stop.

**With the demo data (for presenting).** Creates a fresh `demo.db` with ready-made accounts; every
password is `demo1234`. See [Demo data](#demo-data) for what each account shows.

```powershell
cd "C:\3rd Sem PBL"
Stop-Process -Name LifeLink -Force -ErrorAction SilentlyContinue
if (-not (Test-Path build)) { cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake }
cmake --build build --config Debug
Remove-Item demo.db -ErrorAction SilentlyContinue
.\build\Debug\LifeLink.exe --seed-demo --db demo.db
Start-Process -FilePath ".\build\Debug\LifeLink.exe" -ArgumentList "--db demo.db" -WorkingDirectory "C:\3rd Sem PBL"
Start-Sleep -Seconds 2
Start-Process "http://127.0.0.1:8080"
```

**With your own data** (`lifelink.db`, created automatically the first time):

```powershell
cd "C:\3rd Sem PBL"
Stop-Process -Name LifeLink -Force -ErrorAction SilentlyContinue
if (-not (Test-Path build)) { cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake }
cmake --build build --config Debug
Start-Process -FilePath ".\build\Debug\LifeLink.exe" -WorkingDirectory "C:\3rd Sem PBL"
Start-Sleep -Seconds 2
Start-Process "http://127.0.0.1:8080"
```

**Run the unit tests** (41 tests, a few seconds):

```powershell
cd "C:\3rd Sem PBL"
cmake --build build --config Debug
.\build\Debug\LifeLinkTests.exe
```

If the project is in another folder (for example a fresh `git clone`), change `C:\3rd Sem PBL` in
the `cd` and `Start-Process` lines. The map picker loads its map and place search from the internet;
everything else works offline. If the browser shows an old design, press `Ctrl + F5`.

## Features

- **Accounts:** register with blood group and location, log in, forgot password (security
  question), profile, change password. Passwords are hashed with PBKDF2; 5 failed attempts lock
  the email for 15 minutes.
- **Receive blood:** choose an open hospital, blood group and units; see compatible, eligible
  donors ranked by distance and reliability; invite them; confirm and rate the donation.
- **Donate:** answer the eligibility questionnaire and accept or decline invitations; switch
  availability on or off; the 90-day rule is enforced.
- **Search radius:** 5, 10, 15, 20 or 25 km from the hospital, or auto-expand 5 → 25 km.
- **Map location picker:** OpenStreetMap with place search and "use my current location".
- **Hospital accounts:** registration, dashboard, opening hours, live availability with the
  30-minute rule, "Notify me when this hospital reopens".
- **Admin approval:** new hospitals stay Pending until an administrator verifies or rejects them.
- **Urgent needs:** hospitals post Critical / Urgent blood needs; nearby eligible donors are alerted.
- **Bell notifications** for invitations, accepted requests, urgent needs and reopened hospitals.
- **Demo data** command and **unit tests** for the radius and closure-time logic.

## Project structure

| Folder | What is in it |
|---|---|
| `engine/` | Matching logic with no web or database code: blood compatibility, eligibility, donors, ranking heap (`MatchRequest`), radius search, hospital availability rules, geo distance |
| `db/` | SQLite: schema and upgrades, users, donors, requests, invitations, ratings, hospitals (`HospitalStore.cpp`), demo data (`DemoData.cpp`) |
| `server/` | Crow web server: API routes (`Routes.cpp`, `HospitalRoutes.cpp`), JSON helpers, input validation, password hashing |
| `web/` | The website: HTML pages, `styles.css`, page scripts, map picker, Leaflet |
| `tests/` | Unit tests and the small test runner |
| `main.cpp` | Starts the server and handles the command-line options |

## Tech stack

C++17, Crow (web server), SQLite, nlohmann/json, Windows BCrypt (PBKDF2 password hashing),
CMake + vcpkg, plain HTML/CSS/JavaScript frontend, Leaflet + OpenStreetMap for the map
(Leaflet 1.9.4 is included in `web/vendor/leaflet`, BSD-2-Clause licence).

> **Windows only.** Password hashing uses the Windows BCrypt API, so the project builds on
> Windows with Visual Studio (MSVC).

## Setup on a new computer (once)

1. Install **Visual Studio** with the *Desktop development with C++* workload, **CMake** and **Git**.
2. Install **vcpkg** in `C:\vcpkg` and the libraries:
   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install crow nlohmann-json sqlite3 --triplet x64-windows
   ```
3. Get the code and build it:
   ```powershell
   git clone https://github.com/SKLynIx7/LifeLink.git
   cd LifeLink
   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   cmake --build build --config Debug
   .\build\Debug\LifeLink.exe
   ```

Stop the server before rebuilding, or Windows cannot replace the running program.

## Command-line options

```powershell
.\build\Debug\LifeLink.exe --port 9000 --db data.db --web-dir web   # another port, database or web folder
.\build\Debug\LifeLink.exe --seed-demo --db demo.db                 # fill a NEW database with demo data
.\build\Debug\LifeLink.exe --create-admin someone@example.com       # make an account an administrator
.\build\Debug\LifeLink.exe --remove-admin someone@example.com
.\build\Debug\LifeLink.exe --engine-demo                            # ranking demo, no server
.\build\Debug\LifeLink.exe --database-demo                          # SQLite demo, in-memory database
```

A `lifelink.db` from an earlier version is upgraded automatically when the server starts (the
schema version is stored in the file: 6 adds address columns, 7 hospital accounts, 8 approval,
admins, urgent needs and reopen alerts), so accounts are kept. Only a database older than version
5 shows "created by an older version of LifeLink"; then delete `lifelink.db` and it is recreated.
Database files (`*.db`) hold accounts and password hashes and are never committed to Git.

## Demo data

`--seed-demo` fills a **separate** database with a ready-made Dehradun setup (the first block in
[Run locally](#run-locally) does this for you). Your real `lifelink.db` is not touched, and the
command refuses any database that already has accounts.

Every demo account's password is `demo1234` (security answer `demo`); the command prints the full
list. The main ones:

| Log in as | What it shows |
|---|---|
| `riya@demo.lifelink` (receiver) | New request at Doon Hospital: A+ lists nearby donors ranked by distance and rating; B- widens 5 → 20 km and finds Kavya in Doiwala |
| `asha@demo.lifelink` (donor) | An invitation from Karan to accept (60 minutes), and an Urgent A+ post nearby |
| `vikram@demo.lifelink` (donor) | The Critical O+ post from Himalayan Blood Bank |
| `admin@demo.lifelink` | Admin page: Green Valley Hospital waiting for approval |
| `bloodbank@demo.lifelink` (hospital login) | Approved blood bank dashboard with two urgent posts |
| `greenvalley@demo.lifelink` (hospital login) | "Waiting for approval" dashboard |

Also included: Sanjay (rated 1 and 2 stars, "Low reliability"), Meera (gave blood 30 days ago, not
listed), Deepak (not available), Rohit in Rishikesh (36 km, beyond the 25 km limit), Synergy
"Closing soon" (4 hours), CMI temporarily closed, Coronation open 09:00–17:00 Mon–Sat. The
invitation and the closing-soon countdown start when the demo data is created, so create it
shortly before presenting. The demo is built in `db/DemoData.cpp` through the normal database
functions.

## Unit tests

`tests/` holds unit tests for the search radius (allowed values, auto-expand 5 → 25 km, fixed
radius, the 25 km limit and the database bounding box) and for hospital availability (opening
hours, working days, overnight shifts, "closing soon" and the 30-minute rule). They use a small
built-in test runner, so nothing extra needs installing. Run `.\build\Debug\LifeLinkTests.exe`
to see each test by name, or `ctest --test-dir build -C Debug --output-on-failure`.

## Web pages (`web/`)

| Page | What it does |
|---|---|
| `login.html` | The first screen, with LifeLink's highlights beside the form. `index.html` sends visitors here, or to the dashboard if already logged in |
| `register.html` | Create an account: name, email, password, **blood group, location** (map picker, or an area from the Dehradun list), security question |
| `forgot.html` | Reset the password with the security question |
| `dashboard.html` | After login: a welcome banner, then two choices, **Receive** and **Donate**, each with a one-line status, and a link to nearby hospitals |
| `receiver.html` | Request blood, choose and invite donors, see who accepted (with phone), confirm the donation, rate the donor |
| `profile.html` | Change name, blood group, location (map picker), mobile number (donors) and password. Opened from the menu under your name |
| `hospitals.html` | Hospitals near you as cards: distance, Open / Closing soon / Closed badge, blood groups, phone, map link, details. Filter by blood group; "Show closed hospitals" adds permanently closed ones |
| `hospital-login.html`, `hospital-register.html` | Hospital accounts (separate from personal accounts). Registration needs the exact location from the map picker |
| `hospital-dashboard.html` | For a logged-in hospital: availability (open, closing soon in N minutes, closed now, permanently closed, with an optional reason) with a live countdown, profile, hours, blood groups and location |
| `admin.html` | For administrators: approve or reject newly registered hospitals (Pending / Verified / Rejected tabs). Opened from the menu under your name |
| `donor.html` | First time: mobile number and last donation date (blood group and area come from registration). After that: urgent needs from hospitals near you, and the requests for you (or "No requests right now") |

Every page after login has a **bell** at the top right. A red dot means something new: a request
waiting for your reply, a donor who accepted your request, an urgent need near you, or a hospital
that reopened. The menu under your
name shows your donor details, the availability switch, and Log out.

`app.js` holds shared helpers (API calls, session, safe HTML building, the header);
`location-picker.js` is the map picker; `hospital-list.js` draws the hospital cards;
`auth.js`, `dashboard.js`, `receiver.js`, `donor.js`, `hospital.js` and `admin.js` belong to their
pages. `styles.css` holds all styling. Pages refresh every
10 seconds, so new requests and replies appear without reloading.

## Matching rules

- **Compatibility:** `unordered_map` from receiver blood group to allowed donor groups (`engine/BloodType.h`).
- **Eligibility:** donor is available and last donated at least 90 days ago.
- **Search radius:** measured from the hospital (haversine distance). The receiver chooses
  5, 10, 15, 20 or 25 km, or **Auto-expand** (the default), which tries 5 → 10 → 15 → 20 → 25 km
  and stops at the first radius with at least one available donor (`engine/RadiusSearch.h`).
  The server never searches beyond 25 km and rejects any other value with a 400 error. The
  database first narrows donors with a latitude/longitude box around the hospital (indexed),
  then checks the exact distance.
- **Hospitals:** six Dehradun hospitals are added automatically (`Database::seedHospitals`); their map positions are approximate.
- **Ranking:** `priority_queue` ordered by
  `0.65 × (1 − distance/25) + 0.35 × (rating/5)²`. The receiver sees donors in this order;
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

## Hospital availability

Worked out on the server for every request (`engine/HospitalStatus.h`), from what the hospital
set in its dashboard, its opening hours and working days, and the current time:

| Situation | Badge | Message | Can be chosen |
|---|---|---|---|
| Open, more than 60 min before closing | **Open** (green) | This hospital is open. | Yes |
| Set to close in more than 30 minutes, or within the last hour of regular hours | **Closing soon** (amber) | This hospital will close in X minutes. | Yes |
| Closes in 30 minutes or less | **Closed** (red) | This hospital is closing soon and is currently unavailable for new donor visits. | No |
| Temporarily closed, or the set closing time has passed | **Closed** (red) | This hospital is temporarily closed. | No |
| Outside opening hours / not a working day | **Closed** (red) | This hospital is closed now. It opens … at HH:MM. | No |
| Permanently closed / inactive | **Permanently Closed** (red) | Hidden unless "Show closed hospitals" is ticked | No |

The request form shows hospitals as cards; closed ones cannot be selected (tapping one explains
why), and the server refuses a request for a hospital that cannot take a visit now, so the rule
cannot be bypassed from the browser. Lists refresh every 30 seconds and the countdown moves every
15 seconds. The six listed Dehradun hospitals are open 24 hours and have no account. "Open 24
hours" is stored as the same opening and closing time; a closing time earlier than the opening
time means the shift runs past midnight.

## Hospital approval (administrators)

A newly registered hospital is **Pending**: it can log in and fill in its details, but it is not
shown to donors or receivers, cannot be chosen for a request and cannot post urgent needs. An
administrator approves it (**Verified**) or rejects it with a reason (**Rejected**). A rejected
hospital that corrects its details is sent back to Pending automatically. Hospitals that existed
before this feature (including the six listed ones) are Verified.

To make an administrator, register the account on the website as usual, then run this once in a
second PowerShell window (the server can keep running):

```powershell
.\build\Debug\LifeLink.exe --create-admin your.email@example.com
```

Log in with that account; the menu under your name now has **Approve hospitals (admin)**.
`--remove-admin <email>` takes the role away. Add `--db <file>` if you use another database file.

## Urgent needs and alerts

- **Emergency blood requirement:** a verified hospital posts the blood group, units (1–20),
  urgency (Critical / Urgent), how long the post stays up (1–72 h, default 24) and an optional
  note. At most 10 active posts per hospital; it can close a post at any time.
- **Donor alerts:** a donor sees a post on the Donate page and in the bell when all of these hold:
  they are available, the 90-day rule allows them to donate today, their blood group can be given
  to the one needed, and the hospital is within 25 km of their saved location.
- **Notify me when this hospital reopens:** on a closed (not permanently closed) hospital's card.
  The bell says when it can take visits again, whether the hospital reopened itself or its opening
  hours began.

## Location picker

**Choose on map** opens a map of Dehradun with a fixed pin in the middle: drag the map, search for
a place (suggestions appear as you type), or press **Use my current location**. The card at the
bottom shows the address of the point under the pin; **Confirm location** saves it. If location
permission is refused, the map opens on Dehradun and says so. Search, address lookup and map
loading each show a message with **Retry** if they fail, and the Dehradun area list still works
without internet.

- Map tiles: OpenStreetMap (`tile.openstreetmap.org`). Place search and addresses: Photon
  (`photon.komoot.io`), a free OpenStreetMap search service. No API key is needed, so there is no
  key to keep secret. Both are called from the browser, so the map needs an internet connection.
- Saved per user: latitude, longitude, a short label (`area`), formatted address, city, state,
  country, postal code and OpenStreetMap place id. Distances to hospitals use the latitude and
  longitude (indexed, see Matching rules).

## API

All routes are under `/api`. Logged-in routes need the header `X-Session-Token: <token>`.
Donor routes also need a donor profile.

| Area | Routes |
|---|---|
| Account | `register`, `login`, `logout`, `me`, `profile`, `change-password`, `security-question`, `forgot-password` |
| Reference data | `hospitals`, `eligibility-questions` |
| Receiving | `requests` (create), `my-requests`, `requests/{id}`, `requests/{id}/candidates`, `requests/{id}/invite`, `requests/{id}/complete`, `requests/{id}/cancel`, `ratings` |
| Donating | `donor/profile`, `donor/invitations`, `donor/availability`, `invitations/{id}/respond` |
| Bell | `notifications`, `notifications/seen` |
| Location | `location/me` (GET), `location/save` (POST), `location/update` (PUT) |
| Hospitals (public) | `hospitals`, `hospitals/nearby` (`?latitude=&longitude=&radius_km=&blood_group=&show_closed=1`), `hospitals/{id}` |
| Admin | `admin/hospitals` (`?status=pending\|verified\|rejected\|all`), `admin/hospitals/{id}/verification` (PUT) |
| Alerts | `emergencies/nearby`, `reopen-alerts`, `hospitals/{id}/reopen-alert` (POST / DELETE) |
| Hospital accounts | `hospitals/register`, `hospitals/login`, `hospitals/logout`, `hospital/me`, `hospitals/{id}/profile` (PUT), `hospitals/{id}/availability` (PUT), `hospitals/{id}/location` (PUT), `hospitals/{id}/emergencies` (POST), `hospital/emergencies`, `hospitals/{id}/emergencies/{post}/close` (POST) |

Hospital routes use the header `X-Hospital-Token` and a hospital can change only its own record
(403 otherwise). Availability body: `{"status": "open" | "temporarily_closed" | "permanently_closed",
"closure_type": "closing_soon" | "closed_now", "closing_minutes": 1–720, "reason": ...}`.
Hospital passwords need 8+ characters with a letter and a number; PIN codes are 6 digits.

`requests` (create), `requests/{id}/candidates` (`?radius_km=`) and `requests/{id}/invite` take an
optional `radius_km`: `"auto"` (default) or 5, 10, 15, 20, 25. Replies include `radius_km` (the radius
used) and `radii_tried`.

Location bodies (`register`, `profile`, `location/save`, `location/update`) need `area`, `latitude`
(−90 to 90) and `longitude` (−180 to 180); `formatted_address`, `city`, `state`, `country`,
`postal_code` and `place_id` are optional.

Sessions last 24 hours. Five failed login or reset attempts lock the email for 15 minutes.
