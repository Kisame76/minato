package io.github.kisame76.minato.test.components;

import android.content.SearchRecentSuggestionsProvider;

/** A provider of recent search suggestions, made by the system when the app starts. */
public class SuggestProvider extends SearchRecentSuggestionsProvider {
    public static final String AUTHORITY = "io.github.kisame76.minato.test.components.suggest";

    public SuggestProvider() {
        setupSuggestions(AUTHORITY, DATABASE_MODE_QUERIES);
    }
}
