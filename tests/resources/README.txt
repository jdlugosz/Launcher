These tiny original assets are test data for Launcher. No third-party artwork.
Regenerate them with Python's standard library: python generate_assets.py
Python is not needed to build or run the tests; all generated assets are checked in.

artwork.svg: 16x16, transparent margins, red left rectangle, blue bottom-right
square, half-opacity green top-right square. No newline after </svg>.
artwork.png: the same shapes as explicit RGBA pixels, stored as an ordinary PNG.
commented.svg: the SVG with a UTF-8 BOM, XML declaration, external doctype and
leading/trailing comments. Detection must not fetch the referenced DTD.
alternate.svg: a yellow square to distinguish types and language entries.
binary.bin: non-image bytes, including embedded zeros and bytes above 127.
broken.svg: non-XML text ending in </svg>; a sniff is not proof of validity.

fixture.rc builds a resource-only DLL. Original asset bytes are the extraction
oracle. Resource Hacker supplies a separate, optional extraction cross-check.
