#ifndef MINIMA_CLI_H
#define MINIMA_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

// Entry point for the Minima command-line interface.
// Returns 0 on success, non-zero on failure.
int minima_cli_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif // MINIMA_CLI_H
