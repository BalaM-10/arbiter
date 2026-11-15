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
| FR-C1 | `contracts/default.yaml`, `host/arbiter/contract.py` `validate()` | `host/tests/test_contract.py::fr_c1_default_contract_compiles_with_every_field`<br>`host/tests/test_contract.py::fr_c1_no_zones_is_a_valid_contract` | host only: `evidence/runs/2026-11-13-host/host-checks-130757.log` | — | Host tool; YAML in degrees, mm, ms |
| FR-C2 | `host/arbiter/contract.py` `validate()`, strict YAML loader | `host/tests/test_contract.py::fr_c2_missing_value_rejected_by_name`<br>`host/tests/test_contract.py::fr_c2_unknown_key_rejected`<br>`host/tests/test_contract.py::fr_c2_duplicate_key_rejected`<br>`host/tests/test_contract.py::fr_c2_wrong_types_rejected`<br>`host/tests/test_contract.py::fr_c2_non_finite_numbers_rejected`<br>`host/tests/test_contract.py::fr_c2_out_of_range_values_rejected`<br>`host/tests/test_contract.py::fr_c2_contradictions_rejected`<br>`host/tests/test_contract.py::fr_c2_every_problem_reported_at_once`<br>`host/tests/test_contract.py::fr_c2_not_utf8_and_not_yaml_rejected`<br>`host/tests/test_contract.py::fr_c2_rounding_is_toward_safety`<br>`host/tests/test_contract.py::fr_c2_cli_reports_problems_and_exits_2` | host only: `evidence/runs/2026-11-13-host/host-checks-130757.log` | — | Host tool; rounding always toward safety |
| FR-C3 | `host/arbiter/table.py` `Table.pack()` (layout read from the C header) | `host/tests/test_contract.py::fr_c3_layout_matches_the_c_header`<br>`host/tests/test_contract.py::fr_c3_table_has_version_crc_and_source_hash`<br>`host/tests/test_contract.py::fr_c3_source_hash_changes_with_any_byte_of_the_source`<br>`host/tests/test_contract.py::fr_c3_compiling_is_deterministic`<br>`host/tests/test_contract.py::fr_c3_c_loader_reads_exactly_what_the_compiler_wrote`<br>`qnx/tests/test_contract_file.c::fr_c3_compiled_table_matches_source_dump` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-130755.log` (host: `evidence/runs/2026-11-13-host/host-checks-130757.log`) | — | Cross-language: the C loader reads the compiled table on the host and on QNX |
| FR-C4 | `qnx/libcontract/arb_contract.c` `arb_contract_load()` | `qnx/tests/test_libcontract.c::fr_c4_crc32_matches_zlib_check_value`<br>`qnx/tests/test_libcontract.c::fr_c4_valid_table_loads`<br>`qnx/tests/test_libcontract.c::fr_c4_null_arguments_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_wrong_length_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_bad_magic_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_version_mismatch_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_table_size_field_mismatch_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_crc_mismatch_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_any_single_byte_change_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_out_of_range_values_rejected`<br>`qnx/tests/test_libcontract.c::fr_c4_output_untouched_on_error`<br>`qnx/tests/test_libcontract.c::fr_c4_max_duration_must_cover_a_full_range_move`<br>`host/tests/test_contract.py::fr_c4_c_loader_rejects_every_corrupted_variant`<br>`host/tests/test_contract.py::fr_c4_c_loader_test_detects_a_wrong_expectation`<br>`qnx/tests/test_contract_file.c::fr_c4_corrupted_compiled_table_rejected`<br>`qnx/tests/guardian_suite.sh::fr_c4_guardian_refuses_corrupted_contract`<br>`qnx/tests/arm_suite.sh::fr_c4_arm_refuses_corrupted_contract` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | Load-time checks done; the guardian and the arm refuse to start on a bad table (on-target tests above) |
| FR-C5 | `qnx/libcontract/arb_contract.c` (binary table only) | `qnx/tests/test_libcontract.c::fr_c5_text_contract_rejected` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-122201.log` (host: `evidence/runs/2026-11-13-host/host-checks-122202.log`) | — | The guardian and the arm load only the binary table, through `arb_contract_load()`; neither contains a text parser. Their refusal of a bad table is tested under FR-C4 |
| FR-C6 | — | — | — | — | Planned: M2, with the evidence packet (FR-E3). The compiler already embeds the source SHA-256 (FR-C3) and the guardian prints its prefix at startup |

## Guardian (FR-G)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-G1 | `qnx/guardian/main.c` `main()` (`resmgr_attach`), `io_devctl()` | `qnx/tests/test_guardian.c::fr_g1_dev_arbiter_exists_in_path_namespace`<br>`qnx/tests/test_guardian.c::fr_g1_command_round_trip_over_devctl` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-141527.log` (host: `evidence/runs/2026-11-13-host/host-checks-141549.log`) | — | Resource manager at `/dev/arbiter`; devctl SUBMIT and GET_STATUS; message checks per the 8.0 System Security Guide |
| FR-G2 | `qnx/common/arb_proto.h` `arb_cmd_t` | `qnx/tests/test_libcontract.c::fr_g2_command_fields_carried_through`<br>`qnx/tests/test_libcontract.c::fr_g2_duration_zero_denied`<br>`qnx/tests/test_libcontract.c::fr_g2_duration_minimum_and_contract_max_allowed`<br>`qnx/tests/test_libcontract.c::fr_g2_duration_one_over_contract_max_denied`<br>`qnx/tests/test_libcontract.c::fr_g2_clamp_beyond_contract_max_denied`<br>`qnx/tests/test_guardian.c::fr_g2_zero_duration_denied_over_devctl` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-141527.log` (host: `evidence/runs/2026-11-13-host/host-checks-141549.log`) | — | Decided Nov 13: commands carry a requested duration, bounded by the contract maximum. Carried over devctl on `/dev/arbiter` (on-target test above) |
| FR-G3 | `qnx/libcontract/arb_contract.c` `arb_guard_check()` | `qnx/tests/test_libcontract.c::fr_g3_every_verdict_names_its_rule`<br>`qnx/tests/test_guardian.c::fr_g3_clamp_and_denials_over_devctl`<br>`qnx/tests/test_guardian.c::fr_g3_stale_start_reported_as_deny` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-141527.log` (host: `evidence/runs/2026-11-13-host/host-checks-141549.log`) | — | Decision logic in libcontract; verdict and rule returned over devctl (on-target tests above) |
| FR-G4 | `qnx/libcontract/arb_contract.c` `arb_guard_check()` | `qnx/tests/test_libcontract.c::fr_g4_sequence_replay_and_old_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_sequence_wraps_around`<br>`qnx/tests/test_libcontract.c::fr_g4_rate_limit_burst_then_deny_then_refill`<br>`qnx/tests/test_libcontract.c::fr_g4_rate_limit_uses_guardian_clock_not_sender_clock`<br>`qnx/tests/test_libcontract.c::fr_g4_rate_limit_sustained_rate_holds`<br>`qnx/tests/test_libcontract.c::fr_g4_joint_limit_exactly_at_max`<br>`qnx/tests/test_libcontract.c::fr_g4_joint_limit_just_inside`<br>`qnx/tests/test_libcontract.c::fr_g4_joint_limit_just_outside_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_impossible_arm_position_fails_closed`<br>`qnx/tests/test_libcontract.c::fr_g4_unusable_contract_fails_closed`<br>`qnx/tests/test_libcontract.c::fr_g4_velocity_exactly_at_limit_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_velocity_over_limit_clamped_by_stretching_duration`<br>`qnx/tests/test_libcontract.c::fr_g4_velocity_clamped_motion_never_exceeds_vmax`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_target_inside_zone_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_path_through_zone_denied_though_endpoints_clear`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_clear_path_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_just_outside_by_less_than_margin_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_just_outside_by_more_than_margin_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_overrides_clamp`<br>`qnx/tests/test_libcontract.c::fr_g4_checks_run_in_the_specified_order`<br>`qnx/tests/test_libcontract.c::fr_g4_forward_kinematics_reference_poses`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_brief_crossing_of_thin_zone_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_exit_only_move_from_inside_allowed`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_from_inside_going_deeper_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_from_inside_not_ending_outside_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_exit_through_another_zone_denied`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_normal_moves_carry_no_exit_flag`<br>`qnx/tests/test_libcontract.c::fr_g4_three_step_decision_matches_one_call`<br>`qnx/tests/test_libcontract.c::fr_g4_pre_check_rejects_without_arm_position`<br>`qnx/tests/test_guardian.c::fr_g4_replay_denied_over_devctl`<br>`qnx/tests/test_guardian.c::fr_g4_rate_limit_over_devctl`<br>`qnx/tests/test_libcontract.c::fr_g4_keepout_margin_includes_start_tolerance` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | All six checks in order; keep-out checked along the whole path; exit-only moves from inside a zone (docs/ARCHITECTURE.md) |
| FR-G5 | `qnx/libcontract/arb_contract.c` `arb_guard_check()`, `arb_guard_tick()`, `arb_guard_reset()` | `qnx/tests/test_libcontract.c::fr_g5_starts_in_hold_and_valid_command_enters_normal`<br>`qnx/tests/test_libcontract.c::fr_g5_silence_past_timeout_enters_hold`<br>`qnx/tests/test_libcontract.c::fr_g5_denied_commands_do_not_reset_silence`<br>`qnx/tests/test_libcontract.c::fr_g5_violation_threshold_enters_safe_stop`<br>`qnx/tests/test_libcontract.c::fr_g5_violations_spread_beyond_window_do_not_stop`<br>`qnx/tests/test_libcontract.c::fr_g5_only_joint_and_keepout_denials_count`<br>`qnx/tests/test_libcontract.c::fr_g5_safe_stop_denies_until_operator_reset`<br>`qnx/tests/test_libcontract.c::fr_g5_replay_protection_survives_reset`<br>`qnx/tests/test_libcontract.c::fr_g5_approval_refused_by_arm_is_not_activity`<br>`qnx/tests/test_libcontract.c::fr_g5_last_client_lost_enters_hold`<br>`qnx/tests/test_guardian.c::fr_g5_silence_enters_hold_over_devctl`<br>`qnx/tests/test_guardian.c::fr_g5_last_client_close_enters_hold`<br>`qnx/tests/test_arm.c::fr_g5_hold_pulse_stops_without_latching`<br>`qnx/tests/test_arm.c::fr_g5_brake_pulse_latches`<br>`qnx/tests/test_e2e.c::fr_g5_guardian_silence_holds_the_arm`<br>`qnx/tests/test_guardian.c::fr_g5_forged_tick_and_trip_pulses_are_ignored_and_counted` | yes: 2026-11-15, `evidence/runs/2026-11-15-qemu/run-004541.log` (host: `evidence/runs/2026-11-15-host/host-checks-004740.log`) | — | State logic in libcontract; wired end to end (guardian HOLD and brake pulses, arm hold and latched brake). Operator reset of SAFE_STOP: M2 |
| FR-G6 | `qnx/guardian/arm_link.c` (`name_open`); `qnx/arm/main.c` `authorized()` (per-connection binding, `ConnectClientInfo`) | `qnx/tests/arb_arm_stub.c::fr_g6_name_attach_uses_own_channel`<br>`qnx/tests/test_arm.c::fr_g6_arm_binds_to_the_first_guardian_connection`<br>`qnx/tests/test_arm.c::fr_g6_arm_rejects_setpoints_from_another_process` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | Arm side done for M1: binds to the first connection with the guardian's user ID and refuses every other connection's setpoints and pulses. While all run as root, binding is what separates them (THREAT_MODEL gap); M2 policy closes it |
| FR-G7 | `qnx/guardian/main.c` `on_tick()` (`arb_wd_client_heartbeat()` every tick); `qnx/common/arb_wd_client.c` | `qnx/tests/test_e2e.c::fr_g7_guardian_sends_a_heartbeat_every_tick` | yes: 2026-11-14, `evidence/runs/2026-11-14-qemu/run-195539.log` (host: `evidence/runs/2026-11-14-host/host-checks-195655.log`) | — | Over 1 s the guardian's tick count and the heartbeats the watchdog accepted from its connection agree tick for tick (QEMU: 998 and 998 in `run-195539.log`; function only, not timing) |
| FR-G8 | — | — | — | — | Planned: M2 (evidence logger) |
| FR-G9 | `qnx/libcontract/arb_contract.c` `arb_guard_fault()` | `qnx/tests/test_libcontract.c::fr_g9_arm_send_timeout_fault_enters_safe_stop`<br>`qnx/tests/test_send_timeout.c::fr_g9_send_timeout_fires_when_server_never_receives`<br>`qnx/tests/test_send_timeout.c::fr_g9_send_timeout_fires_when_server_never_replies`<br>`qnx/tests/test_send_timeout.c::fr_g9_unblock_flag_lets_server_delay_timed_out_client`<br>`qnx/tests/test_libcontract.c::fr_g9_arm_fault_denial_carries_no_setpoint`<br>`qnx/tests/test_guardian.c::fr_g9_arm_never_replies_times_out_and_safe_stops`<br>`qnx/tests/test_guardian.c::fr_g9_arm_not_receiving_times_out_and_safe_stops`<br>`scripts/check-bounded-sends.py::fr_g9_timer_timeout_immediately_precedes_send` | yes: 2026-11-15, `evidence/runs/2026-11-15-qemu/run-004541.log` (host: `evidence/runs/2026-11-15-host/host-checks-004740.log`) | — | Fault logic in libcontract; guardian's bounded send tested with a stand-in arm that never replies (3.102 ms) and one that never receives (2.412 ms) in `run-141527.log`; source check that every MsgSend is preceded by TimerTimeout |

## Arm controller (FR-A)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-A1 | `qnx/arm/main.c` (`timer_create` 1 ms pulse, `control_cycle()`) | `qnx/tests/test_arm.c::fr_a1_control_loop_period_measured`<br>`qnx/tests/test_arm.c::fr_a1_forged_tick_pulses_do_not_drive_the_loop` | yes: 2026-11-15, `evidence/runs/2026-11-15-qemu/run-004541.log` (host: `evidence/runs/2026-11-15-host/host-checks-004740.log`) | — | QEMU period measured (function only): median ~1 ms; Pi 5 numbers in M3 |
| FR-A2 | `qnx/arm/arm_motion.c` | `qnx/tests/test_arm_motion.c::fr_a2_model_reports_position_and_velocity_per_joint` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | Ideal simulated actuator; position and velocity per joint |
| FR-A3 | `qnx/arm/arm_motion.c`; `qnx/arm/main.c` `handle_setpoint()` (own vmax and limit checks) | `qnx/tests/test_arm.c::fr_a3_arm_enforces_vmax_itself_and_moves_in_a_straight_line`<br>`qnx/tests/test_arm.c::fr_a3_arm_rejects_targets_beyond_joint_limits`<br>`qnx/tests/test_arm.c::fr_a3_stale_start_rejected`<br>`qnx/tests/test_arm.c::fr_a3_new_setpoint_mid_move_starts_from_actual_position`<br>`qnx/tests/test_arm_motion.c::fr_a3_motion_reaches_target_at_its_duration`<br>`qnx/tests/test_arm_motion.c::fr_a3_motion_is_a_straight_line_with_joints_arriving_together`<br>`qnx/tests/test_arm_motion.c::fr_a3_never_faster_than_vmax_at_the_minimum_duration`<br>`qnx/tests/test_arm_motion.c::fr_a3_min_duration_matches_the_guardian_rule`<br>`qnx/tests/test_arm_motion.c::fr_a3_brake_stays_on_segment`<br>`qnx/tests/test_arm_motion.c::fr_a3_hold_stops_without_latching_and_brake_upgrades_it`<br>`qnx/tests/test_arm_motion.c::fr_a3_new_segment_starts_from_the_given_actual_position`<br>`qnx/tests/test_arm_motion.c::fr_a3_motion_depends_on_time_not_on_tick_count`<br>`qnx/tests/test_e2e.c::fr_a3_guardian_approved_command_moves_the_arm` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | Straight joint-space line, joints arrive together; the arm enforces vmax and joint limits itself; new setpoint starts from the actual position; stale start refused |
| FR-A4 | `qnx/arm/main.c` (motion lease, disconnect brake); `qnx/guardian/main.c` (renewals) | `qnx/tests/test_arm.c::fr_a4_lease_expires_without_renewal_and_brakes`<br>`qnx/tests/test_arm.c::fr_a4_renewal_after_expiry_does_not_resume`<br>`qnx/tests/test_arm.c::fr_a4_renewal_from_another_process_ignored`<br>`qnx/tests/test_arm.c::fr_a4_guardian_disconnect_brakes_at_once`<br>`qnx/tests/test_arm_motion.c::fr_a4_brake_from_full_speed_stops_within_its_braking_time`<br>`qnx/tests/test_e2e.c::fr_a4_guardian_killed_arm_brakes`<br>`qnx/tests/test_e2e.c::fr_a4_lease_expires_when_guardian_stopped` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | Lease of 10 ms renewed by the guardian; latches; guardian disconnect brakes at once; end to end with the real guardian killed or stopped |
| FR-A5 | `qnx/arm/arm_backend.h`, `qnx/arm/arm_backend_sim.c` | `qnx/tests/test_arm_motion.c::fr_a5_simulation_sits_behind_the_backend_interface` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | One backend (simulation) in M1 |
| FR-A6 | `qnx/common/arb_telemetry.h`; `qnx/arm/main.c` `publish()` | `qnx/tests/test_arm.c::fr_a6_telemetry_published_every_cycle` | yes: 2026-11-15, `evidence/runs/2026-11-15-qemu/run-004541.log` (host: `evidence/runs/2026-11-15-host/host-checks-004740.log`) | — | Read-only shared-memory ring, one sample per cycle; version 2 (Nov 13) adds `t_ns`, the time each position was computed for, so the bench derives speed from telemetry alone |

## Watchdog (FR-W)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| FR-W1 | `qnx/watchdog/main.c` (FIFO 66), `qnx/common/arb_prio.h` | `qnx/tests/watchdog_suite.sh::fr_w1_watchdog_runs_above_the_guardian_and_below_the_arm`<br>`qnx/tests/arm_suite.sh::fr_w1_trusted_processes_refuse_to_start_without_the_watchdog` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-150602.log` (host: `evidence/runs/2026-11-13-host/host-checks-150717.log`) | — | Reworded Nov 13: above the guardian, below only the arm's loop. Arm and guardian refuse to start without the watchdog |
| FR-W2 | `qnx/watchdog/main.c` `trip()` (`MsgDeliverEvent`); `qnx/arm/main.c` (brake on the watchdog's event; `g_wd_tripped` refuses every later setpoint); `qnx/common/arb_wd_client.c` | `qnx/tests/test_watchdog.c::fr_w2_missed_guardian_heartbeat_brakes_the_arm`<br>`qnx/tests/test_watchdog.c::fr_w2_heartbeats_from_another_connection_ignored`<br>`qnx/tests/test_e2e.c::fr_w2_watchdog_records_a_stuck_guardian_and_brakes_the_arm`<br>`qnx/tests/test_e2e.c::fr_w2_arm_stays_braked_after_a_stuck_guardian_resumes`<br>`qnx/tests/test_e2e.c::fr_w2_arm_death_puts_the_guardian_in_safe_stop` | yes: 2026-11-14, `evidence/runs/2026-11-14-qemu/run-190222.log` (host: `evidence/runs/2026-11-14-host/host-checks-190420.log`) | — | Guardian stuck or dead -> the arm's registered brake event; arm dead or stuck -> the guardian's event (SAFE_STOP, cause WATCHDOG). Judge-review M1-J1 (fixed): a stuck guardian that resumes can no longer restart motion; the arm refuses its setpoints, so the guardian enters SAFE_STOP. Before the fix the new test failed on QNX (the arm moved again) |
| FR-W3 | `qnx/watchdog/main.c` (`procmgr_event_notify_add` death notices; heartbeat timeout; status record) | `qnx/tests/test_watchdog.c::fr_w3_stuck_detected_by_missed_heartbeat`<br>`qnx/tests/test_watchdog.c::fr_w3_death_detected_by_death_notice`<br>`qnx/tests/test_watchdog.c::fr_w3_forged_death_notice_ignored`<br>`qnx/tests/test_watchdog.c::fr_w3_live_role_cannot_be_registered_twice`<br>`qnx/tests/test_e2e.c::fr_w3_watchdog_records_that_the_guardian_died` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-150602.log` (host: `evidence/runs/2026-11-13-host/host-checks-150717.log`) | — | Records died / stuck / disconnected; death notices authenticated by the watchdog's own connection (forged ones ignored); heartbeats only from the role's connection |
| FR-W4 | — | — | — | — | Planned: M2 |
| FR-W5 | `qnx/guardian/main.c` (`_NTO_CHF_COID_DISCONNECT`, `on_coid_death()`, `watchdog_lost()`); `qnx/arm/main.c` (`handle_pulse()` `_PULSE_CODE_COIDDEATH`, `watchdog_lost()`); `qnx/common/arb_wd_client.c` `arb_wd_client_server_gone()` (`ConnectServerInfo()` check) | `qnx/tests/test_e2e.c::fr_w5_watchdog_killed_mid_move_guardian_safe_stops_and_arm_brakes`<br>`qnx/tests/test_e2e.c::fr_w5_nothing_moves_after_the_watchdog_died` | yes: 2026-11-15 at 7fb8eaa, `evidence/runs/2026-11-15-qemu/run-142336.log` (host: `evidence/runs/2026-11-15-host/host-checks-143511.log`); failed before the fix: `evidence/runs/2026-11-15-qemu/run-110326.log` (at b3903f6) | — | Each side decides on its own: the arm brakes and latches, the guardian enters SAFE_STOP with cause WATCHDOG; each records the cause (connection died). Also caught by the missing beat (FR-W6) if the pulse were lost. Motion resumes only after a restart (operator reset: M2 P4). DESIGN_M1_ADDENDUM decision 8 |
| FR-W6 | `qnx/watchdog/main.c` `handle_pulse()` (beat event delivered every 2 ms with `MsgDeliverEvent()`); `qnx/common/arb_wd_client.c` (second registered event, `arb_wd_client_beat()`, `arb_wd_client_tick_lost()`); `qnx/arm/main.c` `control_cycle()`; `qnx/guardian/main.c` `on_tick()`, `on_wd_beat()` | `qnx/tests/test_e2e.c::fr_w6_watchdog_stopped_mid_move_guardian_safe_stops_and_arm_brakes`<br>`qnx/tests/test_e2e.c::fr_w6_resumed_watchdog_does_not_restart_motion`<br>`qnx/tests/test_e2e.c::fr_w6_forged_watchdog_beats_are_ignored`<br>`qnx/tests/test_arm.c::fr_w6_arm_refuses_setpoints_after_losing_the_watchdog` | yes: 2026-11-15 at 7fb8eaa, `evidence/runs/2026-11-15-qemu/run-142336.log` (host: `evidence/runs/2026-11-15-host/host-checks-143511.log`); failed before the fix: `evidence/runs/2026-11-15-qemu/run-110326.log` (at b3903f6) | — | Beat every 2 ms, lost after 10 of the client's own 1 ms ticks without one (owner decision D7; QEMU function only, Pi 5 values in M3). Counted in the client's own ticks, so a late tick of its own does not look like a silent watchdog. A tripped client gets no more beats. Forged beats (wrong token) are ignored and counted. Limit: a watchdog spinning at 66 would starve the guardian (64); the arm (70) still brakes. DESIGN_M1_ADDENDUM decision 9 |

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
| FR-B1 | `host/arbiter/bench/runner.py` `run_bench()`; `qnx/tests/arb_inject.c`; `qnx/tests/bench_scenario.sh`; `scripts/target-ssh.sh` | `host/tests/test_bench.py::fr_b1_scenario_scripts_are_valid_for_arb_inject` | yes: 2026-11-15, `evidence/bench/2026-11-15-qemu/004658/report.html` (bench 5/5 at 8c85fc5) | — | `arbiter bench run --target qemu` runs the M1 set (F1, F4, F5, F6, F9) and writes report.json and report.html; the other ten scenarios in M2 |
| FR-B2 | `host/arbiter/bench/checks.py`, `kinematics.py`, `spec.py`; `PASS_CONDITION` in each `host/arbiter/bench/scenarios/f*.py` | `host/tests/test_bench.py::fr_b2_bench_kinematics_match_the_arm_geometry`<br>`host/tests/test_bench.py::fr_b2_bench_reads_limits_from_the_contract_as_written`<br>`host/tests/test_bench.py::fr_b2_keepout_check_fails_when_the_elbow_enters_the_zone`<br>`host/tests/test_bench.py::fr_b2_keepout_check_catches_a_crossing_between_two_samples`<br>`host/tests/test_bench.py::fr_b2_keepout_check_passes_beside_the_zone`<br>`host/tests/test_bench.py::fr_b2_speed_check_fails_above_the_limit`<br>`host/tests/test_bench.py::fr_b2_speed_check_allows_only_integer_rounding`<br>`host/tests/test_bench.py::fr_b2_speed_check_uses_sample_times_not_nominal_period`<br>`host/tests/test_bench.py::fr_b2_joint_limit_check_fails_one_step_past_the_limit`<br>`host/tests/test_bench.py::fr_b2_telemetry_check_fails_on_a_lost_sample`<br>`host/tests/test_bench.py::fr_b2_on_segment_check_fails_when_the_arm_heads_elsewhere`<br>`host/tests/test_bench.py::fr_b2_verdict_check_fails_on_the_wrong_rule`<br>`host/tests/test_bench.py::fr_b2_every_scenario_states_its_pass_condition`<br>`host/tests/test_bench.py::fr_b2_parser_reads_arb_inject_output`<br>`host/tests/test_bench.py::fr_b2_contract_check_needs_the_full_hash_from_guardian_and_arm`<br>`host/tests/test_bench.py::fr_b2_unexpected_fault_counters_fail_the_baseline`<br>`host/tests/test_bench.py::fr_b2_a_stopped_trusted_process_fails_the_running_check` | host only (judge tests) | — | Pass conditions stated up front; safety decided from telemetry with independent kinematics; each check shown to fail on bad telemetry |
| FR-B3 | `host/arbiter/bench/runner.py` (exit 1 on any failure) | `host/tests/test_bench.py::fr_b3_bench_exits_nonzero_when_a_scenario_fails` | yes: 2026-11-15, `evidence/bench/2026-11-15-qemu/004658/report.html` (bench 5/5 at 8c85fc5) | — | Unattended; exit status 0 only if every scenario passed |

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
| NFR-I1 | — | — | — | — | Planned: M2, with the gateway (the trusted processes are already separate processes sharing only the read-only telemetry) |
| NFR-I2 | — | — | — | — | Decision in M2: `mlockall()` is called but is a no-op in 8.0 (all memory is locked); stacks are not pre-faulted yet |
| NFR-I3 | `qnx/common/arb_prio.h`; `qnx/guardian/main.c` (`pthread_setschedparam`) | `qnx/tests/guardian_suite.sh::nfr_i3_guardian_runs_fifo_at_its_documented_priority`<br>`qnx/tests/arm_suite.sh::nfr_i3_arm_runs_fifo_at_its_documented_priority` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-144326.log` (host: `evidence/runs/2026-11-13-host/host-checks-144412.log`) | — | Guardian FIFO 64 and arm FIFO 70 checked with `pidin` (tests above); the watchdog's FIFO 66 is checked by `fr_w1_watchdog_runs_above_the_guardian_and_below_the_arm` (FR-W1). Re-check after client traffic: judge-review M1-J5 |
| NFR-I4 | — | — | — | — | Planned: M2 |
| NFR-I5 | — | — | — | — | Planned: M2 |
| NFR-I6 | `qnx/libcontract/arb_contract.c` `arb_guard_check()` | `qnx/tests/test_libcontract.c::nfr_i6_command_wrong_length_rejected`<br>`qnx/tests/test_libcontract.c::nfr_i6_command_bad_fields_rejected`<br>`qnx/tests/test_libcontract.c::nfr_i6_random_bytes_never_crash_or_allow_garbage`<br>`qnx/tests/test_guardian.c::nfr_i6_devctl_wrong_size_rejected`<br>`qnx/tests/test_guardian.c::nfr_i6_unknown_devctl_command_rejected`<br>`qnx/tests/test_guardian.c::nfr_i6_read_only_descriptor_cannot_submit`<br>`qnx/tests/test_guardian.c::nfr_i6_random_payloads_never_crash_the_guardian`<br>`qnx/tests/test_arm.c::nfr_i6_arm_refuses_a_message_too_short_to_hold_a_type` | yes: 2026-11-15, `evidence/runs/2026-11-15-qemu/run-004541.log` (host: `evidence/runs/2026-11-15-host/host-checks-004740.log`) | — | Command side in libcontract; devctl handler side on QNX (wrong sizes, unknown commands, read-only descriptor, random payloads); F8 in the bench: M2 |

## Evidence of QNX use (NFR-Q)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| NFR-Q1 | — | — | — | — | Planned: Ongoing (docs/QNX_EVIDENCE.md) |
| NFR-Q2 | `qnx/guardian/main.c` (resource manager, `io_devctl()`), `qnx/guardian/arm_link.c` (`MsgSend()`), `SIGEV_PULSE_INIT` timers in guardian, arm and watchdog, `MsgSendPulse()` heartbeats and lease signals | `scripts/check-ipc.py::nfr_q2_trusted_code_uses_only_native_ipc`<br>`qnx/tests/test_e2e.c::nfr_q2_each_command_is_one_synchronous_request_and_reply` | yes: 2026-11-14, `evidence/runs/2026-11-14-qemu/run-195539.log` (host: `evidence/runs/2026-11-14-host/host-checks-195655.log`) | — | Source half: no sockets, pipes or message queues in trusted code; every timer delivers a pulse; heartbeats and lease/hold/brake are pulses; the setpoint is a `MsgSend()` (shown to fail on injected violations). Run-time half: when `devctl()` returns, the command is decided and, for ALLOW, the arm has taken the setpoint |
| NFR-Q3 | `qnx/guardian/main.c` `resmgr_attach("/dev/arbiter")` | `qnx/tests/guardian_suite.sh::nfr_q3_guardian_is_a_resource_manager_in_the_path_namespace` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-141527.log` (host: `evidence/runs/2026-11-13-host/host-checks-141549.log`) | — | `ls -l /dev/arbiter` shows a named special file (`n`) owned by the guardian |
| NFR-Q4 | — | — | — | — | Planned: M2 |
| NFR-Q5 | — | — | — | — | Planned: M2 |
| NFR-Q6 | `qnx/common/arb_log.h` (`slog2_register()` once at startup, `slog2c()`/`slog2fa()` with bounded retries); `log_changes()` in `qnx/guardian/main.c`, `qnx/arm/main.c`, `qnx/watchdog/main.c` | `qnx/tests/test_e2e.c::nfr_q6_trusted_processes_log_through_slog2`<br>`qnx/tests/arm_suite.sh::nfr_q6_trusted_processes_run_without_slogger2` | yes: 2026-11-15 at 087b609, `evidence/runs/2026-11-15-qemu/run-102857.log` lines 354-370 (the slog2info lines it checked) (host: `evidence/runs/2026-11-15-host/host-checks-103104.log`) | — | Buffer sets `arb_guardian`, `arb_arm`, `arb_watchdog` (`slog2info -b <set>`). Logged: startup and contract hash, startup failures, guardian state changes with cause, arm bindings, refused senders, brakes with their cause, watchdog registrations and trips with cause. Written by each server loop after the message is handled and replied to, never on the decision path; repeated events logged 16 times, then once per 1024; `nfr_q6_trusted_processes_run_without_slogger2`: first passed at b3903f6, `evidence/runs/2026-11-15-qemu/run-110326.log` (slogger2 stopped, all three print `running without the system log`, the move tests pass, slogger2 restored) |

## Quality (NFR-K)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| NFR-K1 | — | — | — | — | Planned: Ongoing (every run log's build section) |
| NFR-K2 | `qnx/libcontract/` | `scripts/check-pure.sh::nfr_k2_libcontract_is_pure` | yes: 2026-11-13, `evidence/runs/2026-11-13-qemu/run-122201.log` (host: `evidence/runs/2026-11-13-host/host-checks-122202.log`) | — | Host (gcc, ASan/UBSan) and QNX; `make -C qnx check-pure` |
| NFR-K3 | — | — | — | — | Planned: Ongoing (README quick start) |
| NFR-K4 | `pyproject.toml` (ruff, mypy --strict, pytest) | `scripts/host-checks.sh::nfr_k4_python_lint_and_types` | n/a (host tools) | n/a | Tests for the compiler now; the verifier (FR-E4) in M2 |
| NFR-K5 | — | — | — | — | Planned: Ongoing |

## Fault scenarios (F)

| Requirement | Implemented in | Test | Passes on QEMU | Passes on Pi 5 | Notes |
| --- | --- | --- | --- | --- | --- |
| F1 | `host/arbiter/bench/scenarios/f1.py`; `host/arbiter/bench/checks.py` `no_missed_cycle()` | `host/arbiter/bench/scenarios/f1.py::f1_pick_and_place_all_allowed_reached_no_deadline_miss`<br>`host/tests/test_bench.py::f1_tick_spacing_is_strict_on_the_pi5_and_on_unknown_targets`<br>`host/tests/test_bench.py::f1_tick_spacing_on_qemu_is_reported_with_the_reason_not_judged`<br>`host/tests/test_bench.py::f1_lease_expiry_or_send_fault_fails_on_every_target` | yes: 2026-11-15, `evidence/bench/2026-11-15-qemu/004658/report.html` (bench 5/5 at 8c85fc5; tick spacing reported, not judged on QEMU) | — (the 2-period tick rule is judged here) | **What QEMU proves and what it does not.** "No deadline miss" (owner-confirmed Nov 13): no lease expiry, no guardian-to-arm send fault, the lease valid whenever the arm moves, and no telemetry interval of 2 control periods or more. On QEMU the first three are judged, and tick spacing is measured and printed in every report but not judged, because QEMU proves function, never timing (NFR-T7). The 2-period rule is a hard check on the Pi 5 and on any target other than QEMU. History, stated plainly: tick spacing was judged on QEMU too until 1 of 4 runs at 0db90c9 failed on 2 of 12,490 intervals at 2.129 ms (`evidence/bench/2026-11-13-qemu/153541`, kept); the owner then decided QEMU reports it instead. This change was made after that failure and applies to QEMU only. Runs 181350 and 190337 also meet the strict rule (0 intervals at 2 periods or more). 181350's report.html carries the earlier, inaccurate "not a relaxed test" sentence (corrected in the generator after judge-review M1-J2; 190337 has the corrected text) |
| F2 | — | — | — | — | Planned: M2 |
| F3 | — | — | — | — | Planned: M2 |
| F4 | `host/arbiter/bench/scenarios/f4.py` | `host/arbiter/bench/scenarios/f4.py::f4_joint_limit_denied_and_never_exceeded` | yes: 2026-11-15, `evidence/bench/2026-11-15-qemu/004658/report.html` (bench 5/5 at 8c85fc5) | — | Shoulder and elbow past their limits from 5 degrees away |
| F5 | `host/arbiter/bench/scenarios/f5.py` | `host/arbiter/bench/scenarios/f5.py::f5_keepout_denied_and_never_entered` | yes: 2026-11-15, `evidence/bench/2026-11-15-qemu/004658/report.html` (bench 5/5 at 8c85fc5) | — | Target inside the area, and a target outside whose path sweeps through it |
| F6 | `host/arbiter/bench/scenarios/f6.py` | `host/arbiter/bench/scenarios/f6.py::f6_overspeed_clamped_and_never_above_limit` | yes: 2026-11-15, `evidence/bench/2026-11-15-qemu/004658/report.html` (bench 5/5 at 8c85fc5) | — | Speed from telemetry positions and `t_ns` (telemetry v2), never the arm's velocity field |
| F7 | — | — | — | — | Planned: M2 |
| F8 | — | — | — | — | Planned: M2 |
| F9 | `host/arbiter/bench/scenarios/f9.py` | `host/arbiter/bench/scenarios/f9.py::f9_replay_denied_and_arm_never_heads_for_it` | yes: 2026-11-15, `evidence/bench/2026-11-15-qemu/004658/report.html` (bench 5/5 at 8c85fc5) | — | Same, older and duplicate sequence numbers, mid-move, aimed elsewhere |
| F10 | — | — | — | — | Planned: M2 |
| F11 | — | — | — | — | Planned: M2 |
| F12 | — | — | — | — | Planned: M2 |
| F13 | — | — | — | — | Planned: M2 |
| F14 | — | — | — | — | Planned: M2 |
| F15 | — | — | — | — | Planned: M2 |
