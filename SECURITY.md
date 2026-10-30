# Security policy

Minato imports files from anywhere, converts and signs native code on the device, and runs Android apps inside its
own process. Security problems in it matter, and reports are welcome.

## Reporting a vulnerability

Please report security problems privately through GitHub's private vulnerability reporting: open this repository's
**Security** tab and choose **Report a vulnerability**. Do not open a public issue, and do not post details anywhere
else until a fix is released. There is no e-mail address for reports.

Please include:

- what an attacker can do, and under which conditions;
- the version or commit you tested;
- the steps to reproduce, ideally with a minimal test file built for the purpose.

Do not attach APKs of real apps, signing certificates, provisioning profiles or anything personal. The report stays
private while it is handled. Fixes go into `main` and the next release; older betas are not patched.

## What is in scope

- The import: parsing of APKs, split sets and containers (ZIP, binary XML, resources, signatures, OBB files), for
  example path traversal, memory errors or resource exhaustion from a crafted file.
- The converter and the signer: anything that makes Minato sign code it should refuse, or sign it wrongly.
- The runtime: a guest app reaching files outside its own storage or the read-only guest system, reaching another
  app's data, or making the host open things without the user's consent.
- The handling of the signing certificate on the Mac and on the device.
- The release artifacts and the build scripts.

## Signing certificates

Signing certificates and private keys never go into the repository; `*.p12` files are ignored. On the device the
certificate lives in the iOS Keychain. During development, `tools/deploy.sh` hands it to the app encrypted with a
one-time password, and no copy stays on disk. If you find a way a certificate or key could leak, report it as a
vulnerability.

## Not supported, and not vulnerabilities

Minato does not bypass Play Integrity, license checks (LVL), anti-cheat or DRM, and will not. Apps that enforce
them refuse to run, which is intended. Requests to work around these protections are closed, and so are reports
that an app detects that it does not run on a certified Android device.
