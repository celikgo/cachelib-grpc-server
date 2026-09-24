"""Guard against silent acceptance or rejection of adverse benchmark evidence."""
import json
import pathlib
import tempfile
import unittest

import landscape_campaign
import render_landscape_report
import run_strong
from release_campaign import GROUPS, ROOT, group, validate_group


class CampaignValidationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.kv = json.loads((ROOT / "bench/strong/runs/rc-primary-c8/hit_1k_c8/r0/grpc/result.json").read_text())
        self.http = json.loads((ROOT / "bench/strong/runs/rc-http/repeated_256k_c8/r0/grpc_adapter/result.json").read_text())

    def write(self, spec, result, error=None):
        path = self.root / spec["cases"][0] / "r0" / spec["engines"][0]
        path.mkdir(parents=True)
        (path / "result.json").write_text(json.dumps(result))
        (path / "correctness.json").write_text('{"basic":"passed","batch_pipeline":"passed"}')
        if error:
            (path / "error.txt").write_text(error)
        return path

    def test_existing_valid_run_passes(self):
        spec = group("primary", "hit_1k_c8", "grpc", 1)
        self.write(spec, self.kv)
        self.assertTrue(validate_group(spec, self.root)["qualified"])

    def test_missing_required_http_field_is_invalid(self):
        spec = group("http", "repeated_256k_c8", "grpc_adapter", 1, kind="http")
        del self.http["client_exit_code"]
        self.write(spec, self.http)
        result = validate_group(spec, self.root)
        self.assertFalse(result["valid"])
        self.assertIn("client_exit_code", result["problems"][0])

    def test_request_error_is_invalid_even_with_result(self):
        spec = group("primary", "hit_1k_c8", "grpc", 1)
        self.kv["errors"] = 1
        self.write(spec, self.kv)
        self.assertFalse(validate_group(spec, self.root)["valid"])

    def test_nonzero_origin_limits_claim_without_invalidating_execution(self):
        spec = group("ram192", "origin_uniform_64k_5ms", "grpc", 1)
        self.kv["origins"] = 5
        self.write(spec, self.kv)
        result = validate_group(spec, self.root)
        self.assertTrue(result["valid"])
        self.assertFalse(result["qualified"])
        self.assertTrue(result["claim_limitations"])

    def test_unstable_headline_warmup_limits_claim(self):
        spec = group("primary", "hit_1k_c8", "grpc", 1)
        self.kv["warmup_stable"] = False
        self.write(spec, self.kv)
        result = validate_group(spec, self.root)
        self.assertTrue(result["valid"])
        self.assertFalse(result["qualified"])

    def test_cold_start_does_not_require_warmup_stability(self):
        spec = group("origin-patterns", "cold_origin_uniform_64k_5ms", "grpc", 1, headline=False)
        self.kv["warmup_stable"] = False
        self.write(spec, self.kv)
        result = validate_group(spec, self.root)
        self.assertTrue(result["qualified"])
        self.assertFalse(result["observations"])

    def test_saturation_drops_remain_valid_observations(self):
        spec = group("offered-saturation", "offered_1k_80k", "grpc", 1, headline=False)
        self.kv["dropped_arrivals"] = 100
        self.kv["client_exit_code"] = 3
        self.write(spec, self.kv, "Client errors, dropped arrivals, or nonzero exit; see result.json.")
        result = validate_group(spec, self.root)
        self.assertTrue(result["qualified"])
        self.assertIn("dropped_arrivals=100", result["observations"][0])

    def test_equal_demand_drops_prevent_headline_claim(self):
        spec = group("offered", "offered_origin_64k_1500", "grpc", 1)
        self.kv["dropped_arrivals"] = 100
        self.kv["client_exit_code"] = 3
        self.write(spec, self.kv, "Client errors, dropped arrivals, or nonzero exit; see result.json.")
        result = validate_group(spec, self.root)
        self.assertTrue(result["valid"])
        self.assertFalse(result["qualified"])

    def test_missing_repetition_is_invalid(self):
        spec = group("primary", "hit_1k_c8", "grpc", 2)
        self.write(spec, self.kv)
        result = validate_group(spec, self.root)
        self.assertFalse(result["valid"])
        self.assertIn("r1", result["problems"][0])

    def test_truncated_timing_is_invalid(self):
        spec = group("primary", "hit_1k_c8", "grpc", 1)
        self.kv["elapsed_s"] = 5
        self.write(spec, self.kv)
        self.assertFalse(validate_group(spec, self.root)["valid"])

    def test_declared_overload_probe_records_drops_as_observations(self):
        spec = group("ssd-offered", "offered_origin_64k_2gib_10k", "grpc_nvm", 1,
                     headline=False, overload_probe=True)
        self.kv["dropped_arrivals"] = 25716
        self.kv["client_exit_code"] = 3
        self.write(spec, self.kv, "Client errors, dropped arrivals, or nonzero exit; see result.json.")
        result = validate_group(spec, self.root)
        self.assertTrue(result["qualified"])
        self.assertIn("dropped_arrivals=25716", result["observations"][0])
        self.assertFalse(result["claim_limitations"])

    def test_request_errors_still_fail_an_overload_probe(self):
        spec = group("ssd-offered", "offered_origin_64k_2gib_10k", "grpc_nvm", 1,
                     headline=False, overload_probe=True)
        self.kv["dropped_arrivals"], self.kv["errors"] = 10, 1
        self.write(spec, self.kv)
        self.assertFalse(validate_group(spec, self.root)["valid"])


class CampaignDefinitionTest(unittest.TestCase):
    """A recorded campaign must keep comparing equal to its own definition."""

    def test_optional_keys_are_absent_unless_requested(self):
        spec = group("primary", "hit_1k_c8", "grpc")
        for key in ("flash_mb", "discard_flash", "overload_probe"):
            self.assertNotIn(key, spec)

    def test_optional_keys_appear_when_requested(self):
        spec = group("ssd-highvolume", "origin_uniform_64k_2gib_c32", "grpc_nvm",
                     flash=4096, discard_flash=True, overload_probe=True)
        self.assertEqual(spec["flash_mb"], 4096)
        self.assertTrue(spec["discard_flash"] and spec["overload_probe"])

    def test_release_definition_carries_no_optional_keys(self):
        # The committed 1.8.0 campaign.json recorded its groups without them.
        for spec in GROUPS:
            for key in ("flash_mb", "discard_flash", "overload_probe"):
                self.assertNotIn(key, spec, f"{spec['name']} would no longer match its record")


class LandscapeDefinitionTest(unittest.TestCase):
    """The landscape campaign must be runnable and renderable before it is run."""

    def test_every_case_exists(self):
        for spec in landscape_campaign.GROUPS:
            if spec["kind"] != "kv":
                continue
            for case in spec["cases"]:
                self.assertIn(case, run_strong.CASES, f"{spec['name']} uses an undefined case")

    def test_every_kv_engine_is_launchable(self):
        known = {"grpc", "grpc_nvm", *run_strong.IMAGES}
        for spec in landscape_campaign.GROUPS:
            if spec["kind"] != "kv":
                continue
            for engine in spec["engines"]:
                self.assertIn(engine, known, f"{spec['name']} uses an unknown engine")

    def test_every_engine_has_server_arguments(self):
        class Budget:
            cache_mb, flash_mb = 1024, 4096
        for engine in ("grpc", "grpc_nvm", *run_strong.IMAGES):
            self.assertTrue(run_strong.server_arguments(engine, Budget()))

    def test_dragonfly_refuses_a_budget_it_cannot_run(self):
        class TooSmall:
            cache_mb, flash_mb = 96, 512
        with self.assertRaises(ValueError):
            run_strong.server_arguments("dragonfly", TooSmall())

    def test_renderer_can_label_every_group_and_engine(self):
        for spec in landscape_campaign.GROUPS:
            self.assertIn(spec["name"], render_landscape_report.TITLES)
            for engine in spec["engines"]:
                self.assertIn(engine, render_landscape_report.LABELS)
                self.assertIn(engine, render_landscape_report.VERSIONS)

    def test_file_tier_groups_size_and_discard_their_file(self):
        for spec in landscape_campaign.GROUPS:
            tier = [e for e in spec["engines"] if e in run_strong.FILE_TIER_ENGINES]
            if tier:
                self.assertTrue(spec.get("discard_flash"),
                                f"{spec['name']} would retain backing files for {tier}")


class FlashUsageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = pathlib.Path(self.temp.name)
        flash = self.run / "flash"
        (flash / "Store").mkdir(parents=True)
        (flash / "navy").write_bytes(b"x" * 8192)
        (flash / "Store" / "segment.0").write_bytes(b"y" * 4096)

    def usage(self):
        return json.loads((self.run / "flash-usage.json").read_text())

    def test_size_is_recorded_including_subdirectories(self):
        run_strong.record_flash_usage(self.run, discard=False)
        self.assertEqual(self.usage()["flash_file_logical_bytes"], 12288)
        self.assertEqual({entry["path"] for entry in self.usage()["files"]},
                         {"navy", "Store/segment.0"})
        self.assertTrue((self.run / "flash").exists())

    def test_discard_reclaims_the_disk_after_recording(self):
        run_strong.record_flash_usage(self.run, discard=True)
        self.assertEqual(self.usage()["flash_file_logical_bytes"], 12288)
        self.assertTrue(self.usage()["discarded_after_measurement"])
        self.assertFalse((self.run / "flash").exists())


if __name__ == "__main__":
    unittest.main()
