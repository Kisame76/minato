package android.app;

import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.SigningInfo;

import com.android.minato.pm.SignersFile;

import java.util.List;

/**
 * Who signed the app, from its signers.json (ADR-0039 b3.5b1, ADR-0034 decision 2), as Android's package manager gives
 * it: the SigningDetails its verifier makes, and what GET_SIGNATURES and GET_SIGNING_CERTIFICATES give. Kept apart from
 * ManifestPackage, so that framework/tests/package-manager's PackageSigningTest checks these rules on the host's JVM
 * with the taken SigningDetails and SigningInfo.
 */
final class PackageSigning {
    private PackageSigning() {
    }

    /**
     * The SigningDetails Android's verifier makes: one Signature per signer, with the scheme (v3.1 is v3's), and for a
     * rotated key its lineage as past signing certificates with their capabilities, oldest first, the signer last.
     * Without an X.509 provider in the boot jars there are no public keys (the 4-argument constructor).
     */
    static SigningDetails details(SignersFile file) {
        List<byte[]> signers = file.signers();
        Signature[] signatures = new Signature[signers.size()];
        for (int i = 0; i < signatures.length; i++) {
            signatures[i] = new Signature(signers.get(i));
        }
        Signature[] past = null;
        if (!file.lineage().isEmpty()) {
            past = new Signature[file.lineage().size()];
            for (int i = 0; i < past.length; i++) {
                SignersFile.Node node = file.lineage().get(i);
                past[i] = new Signature(node.certificate());
                past[i].setFlags(node.flags);
            }
        }
        int scheme = file.scheme == 31 ? SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3 : file.scheme;
        return new SigningDetails(signatures, scheme, null, past);
    }

    /**
     * GET_SIGNATURES as PackageInfoCommonUtils gives it (198-211): with a lineage its oldest certificate, so that checks
     * unaware of key rotation keep working, otherwise the signers; null for an unsigned app.
     */
    static Signature[] signatures(SigningDetails details) {
        if (details.hasPastSigningCertificates()) {
            return new Signature[] {details.getPastSigningCertificates()[0]};
        }
        return details.hasSignatures() ? details.getSignatures().clone() : null;
    }

    /** GET_SIGNING_CERTIFICATES as PackageInfoCommonUtils gives it (213-221): null for an unsigned app. */
    static SigningInfo signingInfo(SigningDetails details) {
        return details != SigningDetails.UNKNOWN ? new SigningInfo(details) : null;
    }
}
