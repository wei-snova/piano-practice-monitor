# Codex handoff

This file carries the important context from the related ChatGPT/Codex conversation. The complete chat transcript is not in this repository.

## Project

- Firmware for the Waveshare ESP32-S3-Touch-LCD-4B (480×480 touchscreen, onboard microphones), using ESP-IDF, LVGL, and the Waveshare BSP.
- The goal is automatic cello/piano practice tracking, with per-instrument daily totals, a Wi-Fi stats page, and session CSV export. See `README.md` for the current implementation and limits.
- Instrument classification is an adaptive, hand-engineered audio-feature model (not TinyML). It uses per-note cues such as decay, timbre change, attack, and vibrato. User confirmation/correction taps during or after a session feed learning. Real instrument performance should be assessed on the family's board; README's test claims are based on synthetic signals.
- Current UI changes include per-instrument configurable goals (default 30 minutes each, saved on board), weekly progress bars scaled to each goal, and a confidence percentage badge.

## Decisions from the conversation

- Keep improving and deploying the adaptive classifier instead of replacing it with TinyML for now; preserve the ability to add cello samples/training later.
- The screen should show confidence as a percentage without repeating the instrument name in the badge.
- User requested daily goals editable independently for cello and piano, default 30 minutes each.
- Previous UI preferences include no “Good Job” pop-up and a fully off display after inactivity due to flicker with dimming. Check current firmware before assuming these display behaviors are present in this repository.
- Do not delete or clean up audio recordings unless explicitly asked. Collection and recordings may be outside this source repository.
- The live board has had intermittent USB serial permission/acknowledgement issues. Do not claim firmware is flashed or verified on hardware unless it is actually checked during this session.

## Repository state

- Canonical repository: https://github.com/wei-snova/piano-practice-monitor
- Default branch: `main`; `origin` now points to the renamed repository.
- Latest commit at handoff: `38b7ae9` (`Add per-instrument goals and confidence badge`), following `686afca`.
- The former `piano-practice-monitor-claude` repository was renamed after the older `piano-practice-monitor` repository was removed.
- The prior agent could not build firmware because its available ESP-IDF installation was incomplete. Do not treat a successful build or hardware flash as established.

## Suggested next step

Read `README.md`, inspect `git status` and recent history, then check whether the current task is to flash the latest firmware, improve/test classifier behavior, or continue another feature. Ask Wei before destructive changes to recordings or board data.
