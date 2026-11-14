# Python and Pillow for export.py. libfribidi is what lets Pillow lay out text with raqm, as it
# does on a Steam Frame, so emoji sequences (flags, families) come out as one picture.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends libfribidi0 && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir pillow==11.3.0
