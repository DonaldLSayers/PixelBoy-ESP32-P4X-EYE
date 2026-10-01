"""Locates the original Python reference implementation that a few tools/host
scripts cross-check the C core against.

That implementation is a separate project, not part of this repo, so those
scripts only run when PIXELBOY_REF points at its root - the folder containing
its `pixelboy` package:

    $env:PIXELBOY_REF = "C:\\path\\to\\that\\project"

Without it the scripts exit with that message rather than an ImportError.
"""
import os
import sys

ENV = "PIXELBOY_REF"


def add_to_path():
    """Put the reference project on sys.path and return its root, or exit with
    a message saying what to set - the whole point of these scripts is the
    comparison against it, so there is nothing useful to do without it."""
    root = os.environ.get(ENV)
    if not root or not os.path.isdir(os.path.join(root, "pixelboy")):
        sys.exit(f"{ENV} must point at the reference project's root (the folder "
                 f"containing its `pixelboy` package) - the cross-check this script "
                 f"runs compares the C core against that implementation, which is "
                 f"not part of this repo.")
    if root not in sys.path:
        sys.path.insert(0, root)
    return root
