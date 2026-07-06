package cli

import (
	"fmt"

	"github.com/spf13/cobra"

	"github.com/arnal/corefw/internal/build"
	"github.com/arnal/corefw/internal/platformio"
	"github.com/arnal/corefw/internal/progress"
)

type buildFlags struct {
	outputDir string
	firmware  string
	noCompile bool
}

func newBuildCommand(deps commandDeps) *cobra.Command {
	var flags buildFlags
	cmd := &cobra.Command{
		Use:     "build <profile>",
		Short:   "Compile firmware",
		Long:    "Prepare a profile-generated PlatformIO project, run PlatformIO, and report the produced firmware artifacts.",
		GroupID: groupFirmware,
		Args:    exactArgs(1),
		Example: `  corefw build profiles/heltec-v3-repeater.yaml
  corefw build heltec-v3-repeater
  corefw --plain build profiles/heltec-v3-repeater.yaml`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			profilePath := resolveProfileArg(args[0])
			res, err := runBuildCommand(cmd, deps, build.Options{
				Context:     cmd.Context(),
				ProfilePath: profilePath,
				OutDir:      flags.outputDir,
				FirmwareDir: flags.firmware,
				Compile:     !flags.noCompile,
			})
			if err != nil {
				return err
			}
			reportGenerated(cmd, res.OutDir, res.Gen.Files)
			reportArtifacts(cmd, res)
			reportPIOLog(cmd, res)
			if flags.noCompile {
				fmt.Fprintln(cmd.OutOrStdout(), "\n`corefw build --no-compile` is deprecated; use `corefw prepare` instead.")
				fmt.Fprintf(cmd.OutOrStdout(), "\nBuild with: %s\n", platformio.ManualCommand(res.OutDir, res.Gen.EnvName, false, ""))
			}
			return nil
		},
	}
	cmd.Flags().StringVar(&flags.outputDir, "out", "", "output directory (default build/<name>)")
	cmd.Flags().StringVar(&flags.firmware, "firmware", "firmware", "path to the corefw C++ firmware tree")
	cmd.Flags().BoolVar(&flags.noCompile, "no-compile", false, "generate the project but do not run PlatformIO")
	return cmd
}

func newPrepareCommand(deps commandDeps) *cobra.Command {
	var flags buildFlags
	cmd := &cobra.Command{
		Use:     "prepare <profile>",
		Short:   "Generate a PlatformIO project without compiling",
		Long:    "Resolve a profile, fetch components, generate the PlatformIO project, and write corefw.lock without running PlatformIO.",
		GroupID: groupFirmware,
		Args:    exactArgs(1),
		Example: `  corefw prepare heltec-v3-repeater
  corefw prepare profiles/heltec-v3-repeater.yaml --out build/custom`,
		ValidArgsFunction: profileCompletion,
		RunE: func(cmd *cobra.Command, args []string) error {
			profilePath := resolveProfileArg(args[0])
			res, err := runBuildCommand(cmd, deps, build.Options{
				Context:     cmd.Context(),
				ProfilePath: profilePath,
				OutDir:      flags.outputDir,
				FirmwareDir: flags.firmware,
			})
			if err != nil {
				return err
			}
			reportGenerated(cmd, res.OutDir, res.Gen.Files)
			fmt.Fprintf(cmd.OutOrStdout(), "\nBuild with: %s\n", platformio.ManualCommand(res.OutDir, res.Gen.EnvName, false, ""))
			return nil
		},
	}
	cmd.Flags().StringVar(&flags.outputDir, "out", "", "output directory (default build/<name>)")
	cmd.Flags().StringVar(&flags.firmware, "firmware", "firmware", "path to the corefw C++ firmware tree")
	return cmd
}

func runBuildCommand(cmd *cobra.Command, deps commandDeps, opts build.Options) (*build.Result, error) {
	opts.Context = cmd.Context()
	if deps.Root.Verbose {
		opts.PIOOutput = cmd.OutOrStdout()
	}
	if shouldUseInteractiveUI(deps, opts) {
		return runBuildUI(opts, buildUIOptions{
			Stdin:       cmd.InOrStdin(),
			Stdout:      cmd.OutOrStdout(),
			Plain:       deps.Root.Plain,
			NoColor:     deps.Root.NoColor,
			Interactive: true,
		}, deps.BuildRunner)
	}
	opts.Reporter = progress.NewPlainReporter(cmd.OutOrStdout())
	return deps.BuildRunner.Run(cmd.Context(), opts)
}

func shouldUseInteractiveUI(deps commandDeps, opts build.Options) bool {
	if !(opts.Compile || opts.Upload) {
		return false
	}
	if deps.Root.Plain || deps.Root.Verbose {
		return false
	}
	if deps.Getenv("CI") != "" {
		return false
	}
	return deps.IsStdoutTerminal()
}

func reportGenerated(cmd *cobra.Command, outDir string, files []string) {
	fmt.Fprintf(cmd.OutOrStdout(), "\nGenerated %s\n", outDir)
	for _, f := range files {
		fmt.Fprintf(cmd.OutOrStdout(), "  %s\n", f)
	}
	fmt.Fprintln(cmd.OutOrStdout(), "  corefw.lock")
}

func reportArtifacts(cmd *cobra.Command, res *build.Result) {
	if res == nil || len(res.Artifacts) == 0 {
		return
	}
	fmt.Fprintf(cmd.OutOrStdout(), "\nBuilt firmware artifacts\n")
	for _, artifact := range res.Artifacts {
		fmt.Fprintf(cmd.OutOrStdout(), "  %s\n", artifact)
	}
}

func reportPIOLog(cmd *cobra.Command, res *build.Result) {
	if res != nil && res.PIOLog != "" {
		fmt.Fprintf(cmd.OutOrStdout(), "\nPlatformIO log: %s\n", res.PIOLog)
	}
}
