import unittest
import os
import sys

# Add the current directory to sys.path to import generate_profiles
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from generate_profiles import parse_profiles

class TestProfileParsing(unittest.TestCase):
    def test_basic_parsing(self):
        content = """
        "Google Pixel 5" to DeviceFingerprint(
            manufacturer = "Google"
            brand = "google"
        )
        """
        profiles = parse_profiles(content)
        self.assertEqual(len(profiles), 1)
        name, fields = profiles[0]
        self.assertEqual(name, "Google Pixel 5")
        self.assertEqual(fields["manufacturer"], "Google")
        self.assertEqual(fields["brand"], "google")

    def test_parentheses_in_name(self):
        content = """
        "Pixel 4a (5G)" to DeviceFingerprint(
            manufacturer = "Google"
            model = "Pixel 4a (5G)"
        )
        """
        profiles = parse_profiles(content)
        self.assertEqual(len(profiles), 1)
        name, fields = profiles[0]
        self.assertEqual(name, "Pixel 4a (5G)")
        self.assertEqual(fields["manufacturer"], "Google")
        self.assertEqual(fields["model"], "Pixel 4a (5G)")

    def test_multiple_profiles(self):
        content = """
        "Profile 1" to DeviceFingerprint(
            field1 = "val1"
        )
        "Profile 2" to DeviceFingerprint(
            field2 = "val2"
        )
        """
        profiles = parse_profiles(content)
        self.assertEqual(len(profiles), 2)
        self.assertEqual(profiles[0][0], "Profile 1")
        self.assertEqual(profiles[1][0], "Profile 2")

    def test_multiline_body(self):
        content = """
        "MultiLine" to DeviceFingerprint(
            field1 = "val1"
            field2 = "val2"
            field3 = "val3"
        )
        """
        profiles = parse_profiles(content)
        self.assertEqual(len(profiles), 1)
        name, fields = profiles[0]
        self.assertEqual(fields["field1"], "val1")
        self.assertEqual(fields["field2"], "val2")
        self.assertEqual(fields["field3"], "val3")

    def test_empty_body_warning(self):
        # This should print a warning but not crash
        content = """
        "Empty" to DeviceFingerprint(
        )
        """
        profiles = parse_profiles(content)
        self.assertEqual(len(profiles), 0)

    def test_malformed_input(self):
        content = 'Some random text "Name" to DeviceFingerprint('
        profiles = parse_profiles(content)
        self.assertEqual(len(profiles), 0)

if __name__ == "__main__":
    unittest.main()
