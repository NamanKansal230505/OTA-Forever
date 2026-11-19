#!/usr/bin/env python3
"""
Creates the firmware signing key pair (EC P-256), once per project.

  ota_signing_key.pem  -> PRIVATE. Store as the GitHub secret OTA_SIGNING_KEY, then delete or keep offline.
  ota_public_key.pem   -> public. Safe to commit; optionally paste into the device setup page.

Requires: pip install cryptography
"""
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

priv_path, pub_path = Path("ota_signing_key.pem"), Path("ota_public_key.pem")
if priv_path.exists():
    raise SystemExit(f"{priv_path} already exists; refusing to overwrite it.")

key = ec.generate_private_key(ec.SECP256R1())
priv_path.write_bytes(key.private_bytes(
    serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
pub_path.write_bytes(key.public_key().public_bytes(
    serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))

print(f"""Created {priv_path} and {pub_path}.

Next:
  1. Add the private key to your repo as a secret named OTA_SIGNING_KEY:
       gh secret set OTA_SIGNING_KEY < {priv_path}
     (or GitHub > Settings > Secrets and variables > Actions > New repository secret)
  2. Do NOT commit {priv_path} (it is in .gitignore). Keep a backup somewhere safe:
     devices only accept builds signed with this key.
""")
