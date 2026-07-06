package cli

import (
	"context"
	"path/filepath"

	"github.com/arnal/corefw/internal/profile"
	"github.com/arnal/corefw/internal/registry"
	"github.com/arnal/corefw/internal/resolve"
	"github.com/arnal/corefw/internal/source"
)

type PlanOptions struct {
	ProfilePath string
	Offline     bool
}

type PlanResult struct {
	Profile  *profile.Profile
	Registry *registry.Registry
	Plan     *resolve.Plan
}

func defaultLoadPlan(ctx context.Context, opts PlanOptions) (*PlanResult, error) {
	_ = ctx
	opts.ProfilePath = resolveProfileArg(opts.ProfilePath)
	p, err := profile.Load(opts.ProfilePath)
	if err != nil {
		return nil, err
	}
	if p.Name == "" {
		p.Name = baseName(opts.ProfilePath)
	}
	reg, err := registry.New()
	if err != nil {
		return nil, err
	}
	if !opts.Offline && len(p.External) > 0 {
		abs, err := filepath.Abs(opts.ProfilePath)
		if err != nil {
			return nil, err
		}
		fetcher := &source.Fetcher{BaseDir: filepath.Dir(abs)}
		if _, err := fetcher.Resolve(reg, p.External); err != nil {
			return nil, err
		}
	}
	plan, err := resolve.Resolve(p, reg)
	if err != nil {
		return nil, err
	}
	return &PlanResult{Profile: p, Registry: reg, Plan: plan}, nil
}
