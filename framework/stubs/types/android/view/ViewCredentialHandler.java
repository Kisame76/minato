package android.view;

import android.credentials.GetCredentialException;
import android.credentials.GetCredentialRequest;
import android.credentials.GetCredentialResponse;
import android.os.OutcomeReceiver;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View's credential manager request
 * (setCredentialManagerRequest).
 */
public class ViewCredentialHandler {
    ViewCredentialHandler(GetCredentialRequest request, OutcomeReceiver<GetCredentialResponse, GetCredentialException> callback) {
        throw new RuntimeException("stub");
    }

    public GetCredentialRequest getRequest() {
        throw new RuntimeException("stub");
    }

    public OutcomeReceiver<GetCredentialResponse, GetCredentialException> getCallback() {
        throw new RuntimeException("stub");
    }
}
