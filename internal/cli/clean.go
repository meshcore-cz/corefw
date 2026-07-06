package cli

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/spf13/cobra"
)

func newCleanCommand(deps commandDeps) *cobra.Command {
	var out string
	var all bool
	var dryRun bool
	cmd := &cobra.Command{
		Use:     "clean [profile]",
		Short:   "Remove generated corefw build output",
		GroupID: groupFirmware,
		Args:    rangeArgs(0, 1),
		Example: `  corefw clean profiles/wio-l1.yaml
  corefw clean profiles/wio-l1.yaml --dry-run
  corefw clean --all`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			if all && len(args) > 0 {
				return usageErrorf("clean accepts either a profile or --all, not both")
			}
			target := out
			if all {
				target = "build"
			}
			if target == "" && len(args) == 1 {
				res, err := deps.PlanLoader.Load(cmd.Context(), PlanOptions{ProfilePath: args[0], Offline: true})
				if err != nil {
					return err
				}
				target = filepath.Join("build", res.Profile.Name)
			}
			if target == "" {
				return usageErrorf("clean requires a profile, --out, or --all")
			}
			path, err := safeCleanPath(deps, target)
			if err != nil {
				return err
			}
			if !all {
				if err := validateGeneratedCleanTarget(path); err != nil {
					return err
				}
			}
			if dryRun {
				fmt.Fprintf(cmd.OutOrStdout(), "Would remove %s\n", path)
				return nil
			}
			if err := os.RemoveAll(path); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Removed %s\n", path)
			return nil
		},
	}
	cmd.Flags().StringVar(&out, "out", "", "generated output directory to remove")
	cmd.Flags().BoolVar(&all, "all", false, "remove the corefw build root")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "print paths without removing them")
	return cmd
}

func safeCleanPath(deps commandDeps, path string) (string, error) {
	if path == "" {
		return "", fmt.Errorf("refusing to remove empty path")
	}
	cleaned := filepath.Clean(path)
	abs, err := filepath.Abs(cleaned)
	if err != nil {
		return "", err
	}
	cwd, err := deps.WorkingDirectory()
	if err != nil {
		return "", err
	}
	home, _ := os.UserHomeDir()
	volume := filepath.VolumeName(abs)
	root := volume + string(filepath.Separator)
	switch abs {
	case root, cwd, home:
		return "", fmt.Errorf("refusing to remove unsafe path %s", abs)
	}
	if abs == filepath.Dir(abs) {
		return "", fmt.Errorf("refusing to remove filesystem root %s", abs)
	}
	return cleaned, nil
}

func validateGeneratedCleanTarget(path string) error {
	info, err := os.Stat(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("refusing to remove non-directory path %s", path)
	}
	if _, err := os.Stat(filepath.Join(path, "corefw.lock")); err == nil {
		return nil
	}
	if _, err := os.Stat(filepath.Join(path, "platformio.ini")); err == nil {
		return nil
	}
	return fmt.Errorf("refusing to remove %s: not a generated corefw output directory", path)
}
