# Traceability

One row per requirement in `docs/REQUIREMENTS_SUMMARY.md`. A requirement with
no test is not implemented. `scripts/check-traceability.py` fails if any ID is
missing, if a row names a test that does not exist, or if a requirement-named
test is not listed under its requirement.

Test references are `file::test`. On-target results point at a saved run log in
`evidence/runs/`. QEMU results prove function, never timing.

## Hackathon constraints (HC)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| HC-1 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-2 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-3 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-4 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-5 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-6 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-7 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-8 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-9 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-10 | — | — | — | — | Planned: Submission (M4-M5) |
| HC-11 | — | — | — | — | Planned: Submission (M4-M5) |

## Safety contract (FR-C)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-C1 | — | — | — | — | Planned: M1 day 2 (contract compiler) |
| FR-C2 | — | — | — | — | Planned: M1 day 2 (contract compiler) |
| FR-C3 | — | — | — | — | Planned: M1 day 2 (contract compiler) |
| FR-C4 | `qnx/libcontract/arb_contract.c` `arb_contract_load()` | `qnx/tests/test_libcontract.c::fr_c4_crc32_matches_zlib_check_value`<br>`qnx/tests/test_libcontract.c::fr_c4_valid_table_loads`<br>`qnx/tests/test_libcontract.c::fr_c4_null_arguments_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_wrong_length_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_bad_magic_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_version_mismatch_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_table_size_field_mismatch_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_crc_mismatch_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_any_single_byte_change_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_out_of_range_values_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_output_untouched_on_error`<br>`qnx/tests/test_libcontract.c::fr_c4_max_duration_must_cover_a_full_range_move` | pending (re-run after commit: tests added) | — | Load-time checks done; guardian refusing to start: M1 day 3-4 |
| FR-C5 | `qnx/libcontract/arb_contract.c` (binary table only) | `qnx/tests/test_libcontract.c::fr_c5_text_contract_rejected` | pending (re-run after commit: tests added) | — | Guardian side: M1 day 3-4 |
| FR-C6 | — | — | — | — | Planned: M1 day 2 (contract compiler) |

## Guardian (FR-G)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-G1 | — | — | — | — | Planned: M1 day 3-5 |
| FR-G2 | `qnx/common/arb_proto.h` `arb_cmd_t` | `qnx/tests/test_libcontract.c::fr_g2_command_fields_carried_through`<br>`qnx/tests/test_libcontract.c::fr_g2_duration_zero_denied`<br>`qnx/tests/test_libcontract.c::fr_g2_duration_minimum_and_contract_max_allowed`<br>`qnx/tests/test_libcontract.c::fr_g2_duration_one_over_contract_max_denied`<br>`qnx/tests/test_libcontract.c::fr_g2_clamp_beyond_contract_max_denied` | pending (re-run after commit: tests added) | — | Decided Nov 13: commands carry a requested duration, bounded by the contract maximum. devctl transport: M1 day 3-4 |
| FR-G3 | `qnx/libcontract/arb_contract.c` `arb_guard_check()` | `qnx/tests/test_libcontract.c::fr_g3_every_verdict_names_its_rule` | pending (re-run after commit: tests added) | — | Decision logic done; returned over devctl: M1 day 3-4 |
| FR-G4 | `qnx/libcontract/arb_contract.c` `arb_guard_check()` | `qnx/tests/test_libcontract.c::fr_g4_sequence_replay_and_old_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_sequence_wraps_around`<br>`qnx/tests/test_libcontract.c::fr_g4_rate_limit_burst_then_deny_then_refill`<br>`qnx/tests/test_libcontract.c::fr_g4_rate_limit_uses_guardian_clock_not_sender_clock`<br>`qnx/tests/test_libcontract.c::fr_g4_rate_limit_sustained_rate_holds`<br>`qnx/tests/test_libcontract.c::fr_g4_joint_limit_exactly_at_max`<br>`qnx/tests/test_libcontract.c::fr_g4_joint_limit_just_inside`<br>`qnx/tests/test_libcontract.c::fr_g4_joint_limit_just_outside_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_impossible_arm_position_fails_closed`<br>`qnx/tests/test_libcontract.c::fr_g4_unusable_contract_fails_closed`<br>`qnx/tests/test_libcontract.c::fr_g4_velocity_exactly_at_limit_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_velocity_over_limit_clamped_by_stretching_duration`<br>`qnx/tests/test_libcontract.c::fr_g4_velocity_clamped_motion_never_exceeds_vmax`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_target_inside_zone_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_path_through_zone_denied_though_endpoints_clear`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_clear_path_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_just_outside_by_less_than_margin_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_just_outside_by_more_than_margin_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_overrides_clamp`<br>`qnx/tests/test_libcontract.c::fr_g4_checks_run_in_the_specified_order`<br>`qnx/tests/test_libcontract.c::fr_g4_forward_kinematics_reference_poses`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_brief_crossing_of_thin_zone_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_exit_only_move_from_inside_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_from_inside_going_deeper_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_from_inside_not_ending_outside_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_exit_through_another_zone_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_normal_moves_carry_no_exit_flag` | pending (re-run after commit: tests added) | — | All six checks in order; keep-out checked along the whole path; exit-only moves from inside a zone (docs/ARCHITECTURE.md) |
| FR-G5 | `qnx/libcontract/arb_contract.c` `arb_guard_check()`, `arb_guard_tick()`, `arb_guard_reset()` | `qnx/tests/test_libcontract.c::fr_g5_starts_in_hold_and_valid_command_enters_normal`<br>`qnx/tests/test_libcontract.c::fr_g5_silence_past_timeout_enters_hold`<br>`qnx/tests/test_libcontract.c::fr_g5_denied_commands_do_not_reset_silence`<br>`qnx/tests/test_libcontract.c::fr_g5_violation_threshold_enters_safe_stop`<br>`qnx/tests/test_libcontract.c::fr_g5_violations_spread_beyond_window_do_not_stop`<br>`qnx/tests/test_libcontract.c::fr_g5_only_joint_and_keepout_denials_count`<br>`qnx/tests/test_libcontract.c::fr_g5_safe_stop_denies_until_operator_reset`<br>`qnx/tests/test_libcontract.c::fr_g5_replay_protection_survives_reset` | pending (re-run after commit: tests added) | — | State logic done; guardian/arm wiring: M1 day 3-6 |
| FR-G6 | — | — | — | — | Planned: M1 day 3-5 |
| FR-G7 | — | — | — | — | Planned: M1 day 3-5 |
| FR-G8 | — | — | — | — | Planned: M2 (evidence logger) |
| FR-G9 | `qnx/libcontract/arb_contract.c` `arb_guard_fault()` | `qnx/tests/test_libcontract.c::fr_g9_arm_send_timeout_fault_enters_safe_stop` | pending (re-run after commit: tests added) | — | Fault logic only; TimerTimeout-bounded send and its on-target test: M1 day 3-4 |

## Arm controller (FR-A)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-A1 | — | — | — | — | Planned: M1 day 4-5 |
| FR-A2 | — | — | — | — | Planned: M1 day 4-5 |
| FR-A3 | — | — | — | — | Planned: M1 day 4-5 |
| FR-A4 | — | — | — | — | Planned: M1 day 6 |
| FR-A5 | — | — | — | — | Planned: M1 day 4-5 |
| FR-A6 | — | — | — | — | Planned: M1 day 4-5 |

## Watchdog (FR-W)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-W1 | — | — | — | — | Planned: M1 day 6 |
| FR-W2 | — | — | — | — | Planned: M1 day 6 |
| FR-W3 | — | — | — | — | Planned: M1 day 6 |
| FR-W4 | — | — | — | — | Planned: M2 |

## Gateway (FR-N)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-N1 | — | — | — | — | Planned: M2 |
| FR-N2 | — | — | — | — | Planned: M2 |
| FR-N3 | — | — | — | — | Planned: M2 |
| FR-N4 | — | — | — | — | Planned: M2 |

## Evidence (FR-E)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-E1 | — | — | — | — | Planned: M2 |
| FR-E2 | — | — | — | — | Planned: M2 |
| FR-E3 | — | — | — | — | Planned: M2 |
| FR-E4 | — | — | — | — | Planned: M2 |
| FR-E5 | — | — | — | — | Planned: M2 |

## Planner (FR-P)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-P1 | — | — | — | — | Planned: M2 |
| FR-P2 | — | — | — | — | Planned: M3 (LLM planner) |
| FR-P3 | — | — | — | — | Planned: M2 |
| FR-P4 | — | — | — | — | Planned: M2 |

## Fault bench (FR-B)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-B1 | — | — | — | — | Planned: M1 day 7 (partial), M2 |
| FR-B2 | — | — | — | — | Planned: M1 day 7 (partial), M2 |
| FR-B3 | — | — | — | — | Planned: M1 day 7 (partial), M2 |

## Dashboard (FR-D)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-D1 | — | — | — | — | Planned: M3 (dashboard) |
| FR-D2 | — | — | — | — | Planned: M3 (dashboard) |
| FR-D3 | — | — | — | — | Planned: M3 (dashboard) |
| FR-D4 | — | — | — | — | Planned: M3 (dashboard) |
| FR-D5 | — | — | — | — | Planned: M3 (dashboard) |
| FR-D6 | — | — | — | — | Planned: M3 (dashboard) |

## Security policy (FR-S)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-S1 | — | — | — | — | Planned: M2 (security policy) |
| FR-S2 | — | — | — | — | Planned: M2 (security policy) |
| FR-S3 | — | — | — | — | Planned: M2 (security policy) |
| FR-S4 | — | — | — | — | Planned: M2 (security policy) |
| FR-S5 | — | — | — | — | Planned: M2 (security policy) |
| FR-S6 | — | — | — | — | Planned: M2 (security policy) |

## Timing (NFR-T)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| NFR-T1 | — | — | — | — | Planned: M3 (Pi 5 measurements) |
| NFR-T2 | — | — | — | — | Planned: M3 (Pi 5 measurements) |
| NFR-T3 | — | — | — | — | Planned: M3 (Pi 5 measurements) |
| NFR-T4 | — | — | — | — | Planned: M3 (Pi 5 measurements) |
| NFR-T5 | — | — | — | — | Planned: M3 (Pi 5 measurements) |
| NFR-T6 | — | — | — | — | Planned: M3 (Pi 5 measurements) |
| NFR-T7 | — | — | — | — | Planned: M3 (Pi 5 measurements) |

## Isolation and robustness (NFR-I)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| NFR-I1 | — | — | — | — | Planned: M1 day 3-6 |
| NFR-I2 | — | — | — | — | Planned: M1 day 3-6 |
| NFR-I3 | — | — | — | — | Planned: M1 day 3-6 |
| NFR-I4 | — | — | — | — | Planned: M2 |
| NFR-I5 | — | — | — | — | Planned: M2 |
| NFR-I6 | `qnx/libcontract/arb_contract.c` `arb_guard_check()` | `qnx/tests/test_libcontract.c::nfr_i6_command_wrong_length_rejected`<br>`qnx/tests/test_libcontract.c::nfr_i6_command_bad_fields_rejected`<br>`qnx/tests/test_libcontract.c::nfr_i6_random_bytes_never_crash_or_allow_garbage` | pending (re-run after commit: tests added) | — | Command side done; devctl handler side: M1 day 3-4; F8: M2 |

## Evidence of QNX use (NFR-Q)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| NFR-Q1 | — | — | — | — | Planned: Ongoing (docs/QNX_EVIDENCE.md) |
| NFR-Q2 | — | — | — | — | Planned: M1 day 3-5 |
| NFR-Q3 | — | — | — | — | Planned: M1 day 3-4 |
| NFR-Q4 | — | — | — | — | Planned: M2 |
| NFR-Q5 | — | — | — | — | Planned: M2 |
| NFR-Q6 | — | — | — | — | Planned: M1 day 3-6 |

## Quality (NFR-K)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| NFR-K1 | — | — | — | — | Planned: Ongoing (every run log's build section) |
| NFR-K2 | `qnx/libcontract/` | `scripts/check-pure.sh::nfr_k2_libcontract_is_pure` | pending (re-run after commit: tests added) | — | Host (gcc, ASan/UBSan) and QNX; `make -C qnx check-pure` |
| NFR-K3 | — | — | — | — | Planned: Ongoing (README quick start) |
| NFR-K4 | — | — | — | — | Planned: M1 day 2 |
| NFR-K5 | — | — | — | — | Planned: Ongoing |

## Fault scenarios (F)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| F1 | — | — | — | — | Planned: M1 day 7 |
| F2 | — | — | — | — | Planned: M2 |
| F3 | — | — | — | — | Planned: M2 |
| F4 | — | — | — | — | Planned: M1 day 7 |
| F5 | — | — | — | — | Planned: M1 day 7 |
| F6 | — | — | — | — | Planned: M1 day 7 |
| F7 | — | — | — | — | Planned: M2 |
| F8 | — | — | — | — | Planned: M2 |
| F9 | — | — | — | — | Planned: M1 day 7 |
| F10 | — | — | — | — | Planned: M2 |
| F11 | — | — | — | — | Planned: M2 |
| F12 | — | — | — | — | Planned: M2 |
| F13 | — | — | — | — | Planned: M2 |
| F14 | — | — | — | — | Planned: M2 |
| F15 | — | — | — | — | Planned: M2 |
