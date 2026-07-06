package profile

import (
	"fmt"
	"strings"
)

// parseShorthand parses the compact source form. Supported:
//
//	github://owner/repo@ref
//	github://owner/repo            (defaults ref to "main")
//	github://pr#418                (a pull request against the platform repo)
//	local://./path  or  ./path     (a local directory)
func (s *Source) parseShorthand(v string) error {
	v = strings.TrimSpace(v)
	switch {
	case strings.HasPrefix(v, "github://pr#"):
		s.Type = "git"
		s.URL = defaultPlatformRepo
		s.Ref = "pull/" + strings.TrimPrefix(v, "github://pr#") + "/head"
		return nil
	case strings.HasPrefix(v, "github://"):
		rest := strings.TrimPrefix(v, "github://")
		repo, ref, _ := strings.Cut(rest, "@")
		if ref == "" {
			ref = "main"
		}
		s.Type = "git"
		s.URL = "https://github.com/" + repo
		s.Ref = ref
		return nil
	case strings.HasPrefix(v, "local://"):
		s.Type = "local"
		s.Path = strings.TrimPrefix(v, "local://")
		return nil
	case strings.HasPrefix(v, "./") || strings.HasPrefix(v, "/") || strings.HasPrefix(v, "../"):
		s.Type = "local"
		s.Path = v
		return nil
	default:
		return fmt.Errorf("unrecognised source shorthand %q", v)
	}
}

// defaultPlatformRepo is where pr# shorthands resolve against.
const defaultPlatformRepo = "https://github.com/coreprotocol/corefw"
