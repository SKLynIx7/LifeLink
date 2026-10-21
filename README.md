# LifeLink

Emergency blood donor matching for Dehradun. A receiver picks a hospital, blood group and number
of units; LifeLink ranks compatible, eligible donors within 20 km, the receiver invites the ones
they choose, and the first donor to accept is matched.

## Tech stack

C++17, Crow (web server), SQLite, nlohmann/json, Windows BCrypt (PBKDF2 password hashing),
CMake + vcpkg, plain HTML/CSS/JavaScript frontend.

> **Windows only.** Password hashing uses the Windows BCrypt API, so the project builds on
> Windows with Visual Studio (MSVC).

## Setup (once per computer)

1. Install **Visual Studio** with the *Desktop development with C++* workload, **CMake** and **Git**.
2. Install **vcpkg** in `C:\vcpkg` and the libraries:
   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install crow nlohmann-json sqlite3 --triplet x64-windows
   ```

## Build and run

```powershell
git clone https://github.com/SKLynIx7/LifeLink.git
cd LifeLink
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
.\build\Release\LifeLink.exe
```

Open **http://127.0.0.1:8080** and keep the window open while using the site (`Ctrl + C` stops it).
The database `lifelink.db` is created automatically on first run. Stop the server before
rebuilding, or Windows cannot replace the running program.

Other options:

```powershell
.\build\Release\LifeLink.exe --port 9000 --db data.db --web-dir web
.\build\Release\LifeLink.exe --engine-demo      # ranking demo, no server
.\build\Release\LifeLink.exe --database-demo    # SQLite demo, in-memory database
```

If you see "created by an older version of LifeLink", delete `lifelink.db`; it is recreated
automatically with the new layout.

## Run code locally

```powershell
cd "C:\3rd Sem PBL"
Stop-Process -Name LifeLink -Force -ErrorAction SilentlyContinue
cmake --build build --config Debug
Start-Process -FilePath ".\build\Debug\LifeLink.exe" -WorkingDirectory "C:\3rd Sem PBL"
Start-Sleep -Seconds 2
Start-Process "http://127.0.0.1:8080"
```

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

All routes are under `/api`. Logged-in routes need the header `X-Session-Token: <token>`.
Donor routes also need a donor profile.

| Area | Routes |
|---|---|
| Account | `register`, `login`, `logout`, `me`, `profile`, `change-password`, `security-question`, `forgot-password` |
| Reference data | `hospitals`, `eligibility-questions` |
| Receiving | `requests` (create), `my-requests`, `requests/{id}`, `requests/{id}/candidates`, `requests/{id}/invite`, `requests/{id}/complete`, `requests/{id}/cancel`, `ratings` |
| Donating | `donor/profile`, `donor/invitations`, `donor/availability`, `invitations/{id}/respond` |
| Bell | `notifications`, `notifications/seen` |

Sessions last 24 hours. Five failed login or reset attempts lock the email for 15 minutes.
