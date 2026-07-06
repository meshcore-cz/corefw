package cli

import (
	"encoding/json"
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
