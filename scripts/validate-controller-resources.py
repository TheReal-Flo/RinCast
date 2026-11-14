"""Validate packaged native controller bindings without requiring SteamVR."""
import json
import re
from pathlib import Path


def validate(root: Path) -> None:
    profile = json.loads((root / "rincast_pico_profile.json").read_text(encoding="utf-8"))
    sources = profile["input_source"]
    for pose in ("grip", "handgrip", "openxr_grip", "tip", "openxr_aim"):
        assert sources[f"/pose/{pose}"]["type"] == "pose", f"Missing native pose {pose}"
    for binding in profile["default_bindings"]:
        # binding_url is relative to the profile, not a {driver}/resource path.
        target = (root / binding["binding_url"]).resolve()
        assert target.is_relative_to(root.resolve()) and target.is_file(), f"Missing local binding: {target}"
        data = json.loads(target.read_text(encoding="utf-8"))
        assert data["controller_type"] == profile["controller_type"]
        for action_set in data["bindings"].values():
            for kind in ("sources", "poses", "haptics", "skeleton"):
                for source in action_set.get(kind, []):
                    match = re.fullmatch(r"/user/hand/(left|right)(/.*)", source["path"])
                    assert match, f"Invalid input path: {source['path']}"
                    hand, component = match.groups()
                    assert component in sources, f"Undefined input: {component}"
                    assert sources[component].get("side", hand) == hand, f"Wrong hand: {source['path']}"
    remapping = profile["remapping"]
    prefix = "{rincast}/input/"
    assert remapping.startswith(prefix)
    mappings = json.loads((root / remapping[len(prefix):]).read_text(encoding="utf-8"))
    assert mappings["to_controller_type"] == profile["controller_type"]
    for layout in mappings["layouts"]:
        for flag in ("simulate_controller_type", "simulate_render_model", "simulate_HMD"):
            assert layout[flag] is False, f"Native mode unexpectedly enables {flag}"
        for source in layout["autoremappings"]:
            component = re.sub(r"^/user/hand/(left|right)", "", source["to"])
            assert component in sources, f"Unknown remapping destination: {component}"


if __name__ == "__main__":
    resources = Path(__file__).resolve().parents[1] / "pc/driver-openvr/package/rincast/resources/input"
    validate(resources)
    print("ALL CONTROLLER RESOURCE CHECKS PASSED")
