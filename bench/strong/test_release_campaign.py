"""Guard against silent acceptance or rejection of adverse benchmark evidence."""
import json
import pathlib
import tempfile
import unittest

from release_campaign import ROOT, group, validate_group


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


if __name__ == "__main__":
    unittest.main()
