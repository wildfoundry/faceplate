# TV-style Faceplate chrome (operator hierarchy)

Page chrome answers TV-operator questions in priority order. The black
inset card is **terminal-only**.

## Structure

```
[ large dataplicity logo — LEFT ]

[ RIGHT band, same top rows: ]
  Online / Offline      ← lead connection status (strong colour)
  hostname              ← which device
  IP … | OS …           ← labeled LAN + build (ASCII " | ")

[ terminal-only inset card — MOTD / login / shell ]

[ quiet footer: Uptime … | Serial … ]   (+ RAUC only if unhealthy)
```

## Exact copy (healthy idle example)

| Region | String |
| --- | --- |
| Status (right, top) | `Online` |
| Hostname (right) | `raspberrypi-cm5-io-board` |
| Facts (right) | `IP 192.168.128.120 \| OS 0.1.0` |
| Footer | `Uptime 12345s \| Serial f8c7a21b609e` |
| Active rail | `Online \| IP 192.168.128.120` |
| Unhealthy RAUC footer prefix | `RAUC A bad \| …` |

Separators are ASCII ` | ` only (no UTF-8 middle dots).

## Constants

| Item | Value |
| --- | --- |
| Brand band | `BRAND_ROWS` = 3 (fits ~96px wordmark) |
| Gap before card | `BRAND_GAP_ROWS` = 1 |
| Footer gap | `FOOTER_GAP_ROWS` = 2 |
| Safe margin | `SAFE_MARGIN_ROWS` = 8 |
| Online colour | mint `(110,231,183)` |
| Offline / RAUC | amber `(251,146,60)` |

## RAUC

Collector reads booted-slot `boot_status`. Display omits RAUC when
booted slot is `good`/`ok`.
