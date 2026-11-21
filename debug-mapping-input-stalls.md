# Debug Session: mapping-input-stalls
- **Status**: [OPEN]
- **progress_status**: collecting
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

Runtime reproduction and log collection are still pending. No hypothesis is confirmed by instrumentation alone.

## Verification Conclusion
No conclusion before runtime evidence. Static code inspection and CI compilation alone cannot verify latency or intermittent stalls.

## Final Evidence Gate
- **Evidence Sources**: Existing project stderr logging; runtime collection pending.
- **progress_status**: collecting
- **Hypothesis Conclusions**: A INCONCLUSIVE; B INCONCLUSIVE; C INCONCLUSIVE; D INCONCLUSIVE.
