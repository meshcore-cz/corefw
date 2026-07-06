package cli

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"

	"github.com/arnal/corefw/internal/resolve"
)

func ids(sels []*resolve.Selected) string {
	if len(sels) == 0 {
		return "(none)"
	}
	out := make([]string, len(sels))
	for i, s := range sels {
		out[i] = s.ID()
	}
	return strings.Join(out, ", ")
}

func policyIDs(sels []*resolve.Selected) string {
	if len(sels) == 0 {
		return "(none)"
	}
	out := make([]string, len(sels))
	for i, s := range sels {
		out[i] = s.Category + "=" + s.ID()
	}
	return strings.Join(out, ", ")
}

func baseName(path string) string {
	b := filepath.Base(path)
	return strings.TrimSuffix(b, filepath.Ext(b))
}

func jsonIndent(v any) ([]byte, error) {
	return json.MarshalIndent(v, "", "  ")
}

func resolveProfileArg(arg string) string {
	if arg == "" {
		return arg
	}
	if _, err := os.Stat(arg); err == nil {
		return arg
	}
	if hasPathSeparator(arg) {
		return arg
	}

	var candidates []string
	profilesDir := findProfilesDir()
	if filepath.Ext(arg) == "" {
		candidates = append(candidates,
			filepath.Join(profilesDir, arg+".yaml"),
			filepath.Join(profilesDir, arg+".yml"),
		)
	} else {
		candidates = append(candidates, filepath.Join(profilesDir, arg))
	}
	for _, candidate := range candidates {
		if _, err := os.Stat(candidate); err == nil {
			return candidate
		}
	}
	return arg
}

func hasPathSeparator(path string) bool {
	return strings.ContainsAny(path, `/\`)
}

func findProfilesDir() string {
	dir, err := os.Getwd()
	if err != nil {
		return "profiles"
	}
	for {
		candidate := filepath.Join(dir, "profiles")
		if info, err := os.Stat(candidate); err == nil && info.IsDir() {
			if rel, err := filepath.Rel(".", candidate); err == nil {
				return rel
			}
			return candidate
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "profiles"
		}
		dir = parent
	}
}
