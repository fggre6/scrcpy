# Debug Session: mapping-input-stalls
- **Status**: [CLOSED — fix verified by runtime log]
- **progress_status**: verified-by-log, fix applied and confirmed
- **Issue**: Mouse motion mapping feels delayed and occasionally stops responding briefly.
- **Project Root**: d:\DSH-GZQ\scrcpy-Cloud to Local Repo
- **Evidence Source**: Existing project stderr logging (LOGW/LOGE); HTTP evidence files are not used because the user forbids creating or deleting files outside the project.
- **Evidence Directory**: Not created. The usual external evidence directory is intentionally unused under the user's standing file constraint.

## Reproduction Steps
1. Run the Windows client with a MOUSE_MOVE motion mapping.
2. Move the mouse rapidly, stop abruptly, reverse direction, and cross motion wrap boundaries.
3. Capture timestamps for mouse events, pending deltas, injections, queue failures, wraps, and discarded capture deltas.
4. A physical Android device and user mouse input are required for end-to-end reproduction.

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | Pending motion is flushed only by a later mouse event, delaying the final delta after movement stops. | High | Low | Pending delta remains nonzero after an event and injection occurs only on a later event/tick. |
| B | The 8 ms throttle produces coarse motion and excessive event-to-injection latency. | High | Low | Injection intervals and delayed event timestamps cluster around or above 8 ms. |
| C | Control-queue congestion drops MOVE messages and causes brief stalls. | High | Medium | Failed MOVE pushes or queue-limit evidence coincides with stalls. |
| D | Wrap UP/DOWN sequencing or capture-related discarded deltas cause brief stalls. | Medium | Medium | Stall windows coincide with wrap sequences or discard_next events. |

## Log Evidence
Instrumentation added without changing motion behavior:
- A: `sc_screen_handle_event()` reports pending motion older than 8 ms specifically on `SC_EVENT_NEW_FRAME`, avoiding ambiguity with a mouse event that may immediately flush it.
- B: every 16th motion injection attempt reports the interval and accumulated delta to limit observer overhead.
- C: droppable messages rejected at the 60-message control-queue limit are reported outside the queue mutex, including the touch action.
- D: relative-capture discarded deltas and boundary wraps are reported.

## Runtime evidence (session=mapping-input-stalls run=pre-fix, 19.67 s, 414 lines)

- **A: CONFIRMED (root cause).** 366 `overdue-motion-pending-on-video-frame` lines;
  elapsed since last injection min 8 ms / p50 35 ms / p90 487 ms / max 852 ms;
  152 lines >100 ms, 35 lines >500 ms, grouped into 6 hang segments. In segment
  #1 the pending value stayed constant at (-0.000417, 0) for 814 ms and was
  never flushed until mapping was disabled. A residual delta therefore waits
  for the *next mouse event* to be injected, so with a stopped or slow pointer
  it hangs for hundreds of ms and is delivered as a late jump.
- **B: symptom of A, not an independent cause.** Injection intervals p50 15 ms /
  p90 32 ms are normal; the two outliers (386 ms, 556 ms) coincide exactly with
  the flush points of the A hang segments — they are the delayed injection
  itself, not throttle misbehaviour.
- **C: RULED OUT.** Zero `control-queue-drop` lines: the 60-message queue never
  filled; MOVE messages were not being dropped.
- **D: RULED OUT as primary cause.** Only 1 discarded capture delta and 2 wraps
  in 19.67 s, far too few to explain 6 hang segments totalling seconds.

## Fix applied (runId=post-fix)

`input_manager.c`: extracted `motion_flush()` from `motion_feed()` and made the
per-frame `sc_input_manager_tick()` flush any accumulated pending delta once the
8 ms throttle has elapsed — no longer dependent on a further mouse event. The
throttle constant was moved above the tick so both paths share it. Diagnostic
logs B and D now report `runId=post-fix` so a verification capture can be
distinguished from the pre-fix run.

## Post-fix verification (session=mapping-input-stalls run=post-fix, 33.05 s, 281 lines)

- **A: FIXED.** 230 lines, but elapsed since last injection dropped to
  min 8 ms / p50 12.5 ms / p90 21.5 ms / max 75.5 ms. `>100 ms` went from
  152 lines to **0**; `>50 ms` only 3 lines. The 800 ms-class hangs are gone:
  a pending delta is now injected within a few frames instead of waiting for
  the next mouse event.
- **B: no longer indicates hangs.** Intervals p50 22 ms / p90 45 ms; the 4
  remaining `>100 ms` readings (517/361/422/514 ms) each had only 0–6 A lines
  before them, with A max stagnation 0–75.5 ms. They are genuine idle gaps
  between injection bursts (sparse mouse input), not accumulated pending
  deltas. The pre-fix B outliers, by contrast, coincided with multi-hundred-ms
  A hangs.
- One `capture-delta-discarded` line still carried the old `pre-fix` tag (the
  D log inside `motion_feed()` was missed in the first retag); retagged to
  `post-fix` so future captures are unambiguous.

## Verification Conclusion
Root cause confirmed and fix verified by runtime logs. The view-mode latency
regression (hundreds-of-ms pending hangs) is resolved. Remaining long B
intervals are expected idle periods driven by sparse mouse input, not a defect.

## Final Evidence Gate
- **Evidence Sources**: pre-fix and post-fix stderr captures from real mouse motion.
- **progress_status**: verified; session closed.
- **Hypothesis Conclusions**: A CONFIRMED and fixed; B symptom of A (idle gaps remain, benign); C RULED OUT; D RULED OUT (minor contributor only).
