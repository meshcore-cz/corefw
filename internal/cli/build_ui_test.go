package cli

import "testing"

func TestIsInteractiveBuildOutputRejectsNonTerminal(t *testing.T) {
	if isInteractiveBuildOutput(-1, func(string) string { return "" }) {
		t.Fatalf("expected invalid fd to be non-interactive")
	}
}

func TestIsInteractiveBuildOutputRejectsCIAndDumbTerminal(t *testing.T) {
	if isInteractiveBuildOutput(-1, func(key string) string {
		if key == "CI" {
			return "true"
		}
		return ""
	}) {
		t.Fatalf("expected CI to be non-interactive")
	}
	if isInteractiveBuildOutput(-1, func(key string) string {
		if key == "TERM" {
			return "dumb"
		}
		return ""
	}) {
		t.Fatalf("expected dumb TERM to be non-interactive")
	}
}
