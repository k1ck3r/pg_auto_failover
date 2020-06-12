/*
 * src/bin/pg_autoctl/cli_service.c
 *     Implementation of a CLI for controlling the pg_autoctl service.
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#include <errno.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "postgres_fe.h"

#include "cli_common.h"
#include "commandline.h"
#include "defaults.h"
#include "fsm.h"
#include "keeper_config.h"
#include "keeper.h"
#include "monitor.h"
#include "monitor_config.h"
#include "pidfile.h"
#include "service_keeper.h"
#include "service_monitor.h"
#include "signals.h"
#include "string_utils.h"
#include "supervisor.h"


static int stop_signal = SIGTERM;

static void cli_service_run(int argc, char **argv);
static void cli_keeper_run(int argc, char **argv);
static void cli_monitor_run(int argc, char **argv);

static int cli_getopt_pgdata_and_mode(int argc, char **argv);

static void cli_service_stop(int argc, char **argv);
static void cli_service_reload(int argc, char **argv);
static void cli_service_status(int argc, char **argv);

static void cli_service_restart(const char *serviceName);
static void cli_service_restart_all(int argc, char **argv);
static void cli_service_restart_postgres(int argc, char **argv);
static void cli_service_restart_listener(int argc, char **argv);
static void cli_service_restart_node_active(int argc, char **argv);

CommandLine service_run_command =
	make_command("run",
				 "Run the pg_autoctl service (monitor or keeper)",
				 CLI_PGDATA_USAGE,
				 CLI_PGDATA_OPTION,
				 cli_getopt_pgdata,
				 cli_service_run);

CommandLine service_stop_command =
	make_command("stop",
				 "signal the pg_autoctl service for it to stop",
				 " [ --pgdata --fast --immediate ]",
				 "  --pgdata      path to data director \n"
				 "  --fast        fast shutdown mode for the keeper \n"
				 "  --immediate   immediate shutdown mode for the keeper \n",
				 cli_getopt_pgdata_and_mode,
				 cli_service_stop);

CommandLine service_reload_command =
	make_command("reload",
				 "signal the pg_autoctl for it to reload its configuration",
				 CLI_PGDATA_USAGE,
				 CLI_PGDATA_OPTION,
				 cli_getopt_pgdata,
				 cli_service_reload);

CommandLine service_status_command =
	make_command("status",
				 "Display the current status of the pg_autoctl service",
				 CLI_PGDATA_USAGE,
				 CLI_PGDATA_OPTION,
				 cli_getopt_pgdata,
				 cli_service_status);

CommandLine service_restart_all =
	make_command("all",
				 "Restart all the pg_autoctl services",
				 CLI_PGDATA_USAGE,
				 CLI_PGDATA_OPTION,
				 cli_getopt_pgdata,
				 cli_service_restart_all);

CommandLine service_restart_postgres =
	make_command("postgres",
				 "Restart the pg_autoctl postgres controller service",
				 CLI_PGDATA_USAGE,
				 CLI_PGDATA_OPTION,
				 cli_getopt_pgdata,
				 cli_service_restart_postgres);

CommandLine service_restart_listener =
	make_command("listener",
				 "Restart the pg_autoctl monitor listener service",
				 CLI_PGDATA_USAGE,
				 CLI_PGDATA_OPTION,
				 cli_getopt_pgdata,
				 cli_service_restart_listener);

CommandLine service_restart_node_active =
	make_command("node-active",
				 "Restart the pg_autoctl keeper node-active service",
				 CLI_PGDATA_USAGE,
				 CLI_PGDATA_OPTION,
				 cli_getopt_pgdata,
				 cli_service_restart_node_active);

static CommandLine *service_restart[] = {
	&service_restart_all,
	&service_restart_postgres,
	&service_restart_listener,
	&service_restart_node_active,
	NULL
};

CommandLine service_restart_commands =
	make_command_set("restart",
					 "Restart pg_autoctl sub-processes (services)", NULL, NULL,
					 NULL, service_restart);


/*
 * cli_service_run starts the local pg_auto_failover service, either the
 * monitor or the keeper, depending on the configuration file associated with
 * the current PGDATA, or the --pgdata argument.
 */
static void
cli_service_run(int argc, char **argv)
{
	KeeperConfig config = keeperOptions;

	if (!keeper_config_set_pathnames_from_pgdata(&config.pathnames,
												 config.pgSetup.pgdata))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	switch (ProbeConfigurationFileRole(config.pathnames.config))
	{
		case PG_AUTOCTL_ROLE_MONITOR:
		{
			(void) cli_monitor_run(argc, argv);
			break;
		}

		case PG_AUTOCTL_ROLE_KEEPER:
		{
			(void) cli_keeper_run(argc, argv);
			break;
		}

		default:
		{
			log_fatal("Unrecognized configuration file \"%s\"",
					  config.pathnames.config);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}
}


/*
 * keeper_cli_fsm_run runs the keeper state machine in an infinite
 * loop.
 */
static void
cli_keeper_run(int argc, char **argv)
{
	Keeper keeper = { 0 };
	KeeperConfig *config = &(keeper.config);
	PostgresSetup *pgSetup = &(keeper.config.pgSetup);
	LocalPostgresServer *postgres = &(keeper.postgres);

	bool missingPgdataIsOk = true;
	bool pgIsNotRunningIsOk = true;
	bool monitorDisabledIsOk = true;

	keeper.config = keeperOptions;

	/* initialize our pgSetup and LocalPostgresServer instances */
	if (!keeper_config_read_file(config,
								 missingPgdataIsOk,
								 pgIsNotRunningIsOk,
								 monitorDisabledIsOk))
	{
		/* errors have already been logged. */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	/* initialize our local Postgres instance representation */
	(void) local_postgres_init(postgres, pgSetup);

	if (!start_keeper(&keeper))
	{
		log_fatal("Failed to start pg_autoctl keeper service, "
				  "see above for details");
		exit(EXIT_CODE_INTERNAL_ERROR);
	}
}


/*
 * cli_monitor_run ensures PostgreSQL is running and then listens for state
 * changes from the monitor, logging them as INFO messages. Also listens for
 * log messages from the monitor, and outputs them as DEBUG messages.
 */
static void
cli_monitor_run(int argc, char **argv)
{
	KeeperConfig options = keeperOptions;

	Monitor monitor = { 0 };
	bool missingPgdataIsOk = false;
	bool pgIsNotRunningIsOk = true;

	/* Prepare MonitorConfig from the CLI options fed in options */
	if (!monitor_config_init_from_pgsetup(&(monitor.config),
										  &options.pgSetup,
										  missingPgdataIsOk,
										  pgIsNotRunningIsOk))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_PGCTL);
	}

	/* Start the monitor service */
	if (!start_monitor(&monitor))
	{
		log_fatal("Failed to start pg_autoctl monitor service, "
				  "see above for details");
		exit(EXIT_CODE_INTERNAL_ERROR);
	}
}


/*
 * service_cli_reload sends a SIGHUP signal to the keeper.
 */
static void
cli_service_reload(int argc, char **argv)
{
	pid_t pid;
	Keeper keeper = { 0 };

	keeper.config = keeperOptions;

	if (read_pidfile(keeper.config.pathnames.pid, &pid))
	{
		if (kill(pid, SIGHUP) != 0)
		{
			log_error("Failed to send SIGHUP to pg_autoctl pid %d: %m", pid);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}
}


/*
 * cli_getopt_pgdata_and_mode gets both the --pgdata and the stopping mode
 * options (either --fast or --immediate) from the command line.
 */
static int
cli_getopt_pgdata_and_mode(int argc, char **argv)
{
	KeeperConfig options = { 0 };
	int c, option_index = 0;
	int verboseCount = 0;

	static struct option long_options[] = {
		{ "pgdata", required_argument, NULL, 'D' },
		{ "fast", no_argument, NULL, 'f' },
		{ "immediate", no_argument, NULL, 'i' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	optind = 0;

	while ((c = getopt_long(argc, argv, "D:fiVvqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
			{
				strlcpy(options.pgSetup.pgdata, optarg, MAXPGPATH);
				log_trace("--pgdata %s", options.pgSetup.pgdata);
				break;
			}

			case 'f':
			{
				/* change the signal to send from SIGTERM to SIGINT. */
				if (stop_signal != SIGTERM)
				{
					log_fatal("Please use either --fast or --immediate, not both");
					exit(EXIT_CODE_BAD_ARGS);
				}
				stop_signal = SIGINT;
				break;
			}

			case 'i':
			{
				/* change the signal to send from SIGTERM to SIGQUIT. */
				if (stop_signal != SIGTERM)
				{
					log_fatal("Please use either --fast or --immediate, not both");
					exit(EXIT_CODE_BAD_ARGS);
				}
				stop_signal = SIGQUIT;
				break;
			}

			case 'V':
			{
				/* keeper_cli_print_version prints version and exits. */
				keeper_cli_print_version(argc, argv);
				break;
			}

			case 'v':
			{
				++verboseCount;
				switch (verboseCount)
				{
					case 1:
					{
						log_set_level(LOG_INFO);
						break;
					}

					case 2:
					{
						log_set_level(LOG_DEBUG);
						break;
					}

					default:
					{
						log_set_level(LOG_TRACE);
						break;
					}
				}
				break;
			}

			case 'q':
			{
				log_set_level(LOG_ERROR);
				break;
			}

			case 'h':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_QUIT);
				break;
			}

			default:
			{
				commandline_help(stderr);
				exit(EXIT_CODE_BAD_ARGS);
				break;
			}
		}
	}

	/* now that we have the command line parameters, prepare the options */
	(void) prepare_keeper_options(&options);

	keeperOptions = options;

	return optind;
}


/*
 * cli_service_stop sends a SIGTERM signal to the keeper.
 */
static void
cli_service_stop(int argc, char **argv)
{
	pid_t pid;
	Keeper keeper = { 0 };

	keeper.config = keeperOptions;

	if (read_pidfile(keeper.config.pathnames.pid, &pid))
	{
		if (kill(pid, stop_signal) != 0)
		{
			log_error("Failed to send %s to pg_autoctl pid %d: %m",
					  strsignal(stop_signal), pid);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}
	else
	{
		log_fatal("Failed to read the keeper's PID at \"%s\"",
				  keeper.config.pathnames.pid);
	}
}


/*
 * cli_service_status displays the status of the pg_autoctl service and the
 * Postgres service.
 */
static void
cli_service_status(int argc, char **argv)
{
	pid_t pid = 0;
	Keeper keeper = { 0 };
	PostgresSetup *pgSetup = &(keeper.config.pgSetup);
	ConfigFilePaths *pathnames = &(keeper.config.pathnames);

	keeper.config = keeperOptions;

	if (!cli_common_pgsetup_init(pathnames, pgSetup))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	if (!file_exists(pathnames->pid))
	{
		log_info("pg_autoctl pid file \"%s\" does not exists", pathnames->pid);

		if (pg_setup_is_running(pgSetup))
		{
			log_fatal("Postgres is running at \"%s\" with pid %d",
					  pgSetup->pgdata, pgSetup->pidFile.pid);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}

	/* ok now we have a pidfile for pg_autoctl */
	if (read_pidfile(pathnames->pid, &pid))
	{
		if (kill(pid, 0) != 0)
		{
			log_error("pg_autoctl pid file contains stale pid %d", pid);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}
	}

	/* and now we know pg_autoctl is running */
	log_info("pg_autoctl is running with pid %d", pid);

	/* add a word about the Postgres service itself */
	if (pg_setup_is_ready(pgSetup, false))
	{
		log_info("Postgres is serving PGDATA \"%s\" on port %d with pid %d",
				 pgSetup->pgdata, pgSetup->pgport, pgSetup->pidFile.pid);
	}
	else
	{
		exit(EXIT_CODE_PGCTL);
	}

	if (outputJSON)
	{
		JSON_Value *js = json_value_init_object();
		JSON_Value *jsPostgres = json_value_init_object();

		JSON_Value *jsPGAutoCtl = json_value_init_object();
		JSON_Object *jsobj = json_value_get_object(jsPGAutoCtl);

		JSON_Object *root = json_value_get_object(js);

		/* prepare both JSON objects */
		json_object_set_number(jsobj, "pid", (double) pid);

		if (!pg_setup_as_json(pgSetup, jsPostgres))
		{
			/* can't happen */
			exit(EXIT_CODE_INTERNAL_ERROR);
		}

		/* concatenate JSON objects into a container object */
		json_object_set_value(root, "postgres", jsPostgres);
		json_object_set_value(root, "pg_autoctl", jsPGAutoCtl);

		(void) cli_pprint_json(js);
	}

	exit(EXIT_CODE_QUIT);
}


/*
 * cli_service_restart sends the TERM signal to the given serviceName, which
 * is known to have the restart policy RP_PERMANENT (that's hard-coded). As a
 * consequence the supervisor will restart the service.
 */
static void
cli_service_restart(const char *serviceName)
{
	ConfigFilePaths pathnames = { 0 };
	LocalPostgresServer postgres = { 0 };

	pid_t pid = -1;
	pid_t newPid = -1;

	if (!cli_common_pgsetup_init(&pathnames, &(postgres.postgresSetup)))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	if (!supervisor_find_service_pid(pathnames.pid, serviceName, &pid))
	{
		log_fatal("Failed to find pid for service name \"%s\"", serviceName);
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_info("Sending the TERM signal to service \"%s\" with pid %d",
			 serviceName, pid);

	if (kill(pid, SIGTERM) != 0)
	{
		log_error("Failed to send SIGHUP to the pg_autoctl pid %d: %m", pid);
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	/* loop until we have a new pid */
	do {
		if (!supervisor_find_service_pid(pathnames.pid, serviceName, &newPid))
		{
			log_fatal("Failed to find pid for service name \"%s\"", serviceName);
			exit(EXIT_CODE_INTERNAL_ERROR);
		}

		if (newPid == pid)
		{
			log_trace("pidfile \"%s\" still contains pid %d for service \"%s\"",
					  pathnames.pid, newPid, serviceName);
		}

		pg_usleep(100 * 1000);  /* retry in 100 ms */
	} while (newPid == pid);

	log_info("Service \"%s\" has been restarted with pid %d",
			 serviceName, newPid);

	fformat(stdout, "%d\n", pid);
}


/*
 * cli_service_restart_postgres sends the TERM signal to the postgres
 * service, which is known to have the restart policy RP_PERMANENT (that's
 * hard-coded). As a consequence the supervisor will restart the service.
 */
static void
cli_service_restart_postgres(int argc, char **argv)
{
	(void) cli_service_restart("postgres");
}


/*
 * cli_service_restart_postgres sends the TERM signal to the monitor
 * listener service, which is known to have the restart policy RP_PERMANENT
 * (that's hard-coded). As a consequence the supervisor will restart the
 * service.
 */
static void
cli_service_restart_listener(int argc, char **argv)
{
	(void) cli_service_restart("listener");
}


/*
 * cli_service_restart_postgres sends the TERM signal to the keeper node
 * active service, which is known to have the restart policy RP_PERMANENT
 * (that's hard-coded). As a consequence the supervisor will restart the
 * service.
 */
static void
cli_service_restart_node_active(int argc, char **argv)
{
	(void) cli_service_restart("node active");
}


/*
 * cli_service_restart_all sends the TERM signal to all the keeper services. We
 * assume that those services have the restart policy RP_PERMANENT, which is
 * hard-coded.
 */
static void
cli_service_restart_all(int argc, char **argv)
{
	ConfigFilePaths pathnames = { 0 };
	LocalPostgresServer postgres = { 0 };

	long fileSize = 0L;
	char *fileContents = NULL;
	char *fileLines[BUFSIZE] = { 0 };
	int lineCount = 0;
	int lineNumber;

	if (!cli_common_pgsetup_init(&pathnames, &(postgres.postgresSetup)))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_CONFIG);
	}

	if (!file_exists(pathnames.pid))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_STATE);
	}

	if (!read_file(pathnames.pid, &fileContents, &fileSize))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_BAD_STATE);
	}

	lineCount = splitLines(fileContents, fileLines, BUFSIZE);

	for (lineNumber = 0; lineNumber < lineCount; lineNumber++)
	{
		char *serviceName = NULL;
		char *separator = NULL;
		pid_t pid;

		/* skip first and second lines: main pid, semaphore id */
		if (lineNumber < 2)
		{
			continue;
		}

		if ((separator = strchr(fileLines[lineNumber], ' ')) == NULL)
		{
			log_debug("Failed to find a space separator in line: \"%s\"",
					  fileLines[lineNumber]);
			continue;
		}

		serviceName = separator + 1;
		*separator = '\0';
		stringToInt(fileLines[lineNumber], &pid);

		log_info("Restarting service \"%s\" with pid %d", serviceName, pid);
	}
}
