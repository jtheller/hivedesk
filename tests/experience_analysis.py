import csv
import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("experience", pathlib.Path(__file__).parents[1] / "scripts/analyze-experience.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Analysis(unittest.TestCase):
    def test_real_events_not_rolling_percentiles_or_fps_filter(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "experience-1.csv"
            header = ["pid", "qpc", "event", "session", "sequence", *[f"v{i}" for i in range(10)]]
            def row(qpc, event, sequence, values=(), session=9):
                return [1, qpc, event, session, sequence, *values, *([0] * (10 - len(values)))]
            rows = [row(1, 0, 0, (1000000,), 0), row(1000, 1, 1), row(2000, 2, 1),
                    row(4000, 3, 1, (0, 0, 0, 0, 0, 10, 100)),
                    row(5000, 4, 4, (800,)), row(6000, 5, 4, (600, 20, 30, 1)),
                    row(14000, 3, 2, (0, 0, 0, 0, 0, 11, 102)),
                    row(1014000, 3, 3, (0, 0, 0, 0, 0, 14, 200)),
                    row(1014001, 6, 0, (7,), 0)]
            with path.open("w", newline="", encoding="utf-8") as file:
                writer = csv.writer(file);writer.writerow(header);writer.writerows(rows)
                file.write("incomplete\n")
            report = module.analyze([path])["sessions"][0]
            metrics = report["metrics"]
            self.assertEqual(metrics["arrival_to_prepared_ms"]["max"], 1)
            self.assertEqual(metrics["prepared_to_present_ms"]["max"], 2)
            self.assertEqual(metrics["present_interval_including_idle_ms"]["samples"], 2)
            self.assertEqual(metrics["present_interval_including_idle_ms"]["max"], 1000)
            self.assertEqual(metrics["input_ack_roundtrip_ms"]["max"], 1)
            self.assertEqual(metrics["input_capture_to_send_ms"]["max"], .2)
            self.assertEqual(metrics["observed_display_interval_vblanks"]["samples"], 1)
            self.assertEqual(metrics["observed_display_interval_vblanks"]["max"], 2)
            self.assertEqual(report["dxgi_unobserved_presents"], 2)
            self.assertEqual(report["trace_lost_rows_for_process"], 7)
            self.assertEqual(module.analyze([path], 99)["sessions"], [])
            self.assertEqual(module.analyze([path])["invalid_rows"][str(path)], 1)


if __name__ == "__main__":
    unittest.main()
