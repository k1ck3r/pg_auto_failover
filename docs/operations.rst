Operating pg_auto_failover
==========================

This section is not yet complete. Please contact us with any questions.

Deployment
----------

pg_auto_failover is a general purpose tool for setting up PostgreSQL
replication in order to implement High Availability of the PostgreSQL
service.

Provisioning
------------

It is also possible to register pre-existing PostgreSQL instances with a
pg_auto_failover monitor. The ``pg_autoctl create`` command honors the
``PGDATA`` environment variable, and checks whether PostgreSQL is already
running. If Postgres is detected, the new node is registered in SINGLE mode,
bypassing the monitor's role assignment policy.

Upgrading pg_auto_failover
--------------------------

When upgrading a pg_auto_failover setup, the procedure is different on the
monitor and on the Postgres nodes:

  - on the monitor, the internal pg_auto_failover database schema might have
    changed and needs to be upgraded to its new definition, porting the
    existing data over. The pg_auto_failover database contains the
    registration of every node in the system and their current state.

	It is not possible to trigger a failover during the monitor update.
	Postgres operations on the Postgres nodes continue normally.

	During the restart of the monitor, the other nodes might have trouble
	connecting. The ``pg_autoctl`` command is designed to retry connecting
	to the monitor and handle errors gracefully. It also checks of the
	expected Postgres replication connections are still being made, and when
	that's the case, nodes will continue operating normally during the
	monitor ugprade.

  - on the Postgres nodes, the ``pg_autoctl`` command connects to the
    monitor every once in a while (every second by default), and then calls
    the ``node_active`` protocol, a stored procedure in the monitor databases.

	When upgrading the monitor, it may happen that this procedure has a new
	API contract. In that case queries from the running ``pg_autoctl`` are
	going to fail. We need to upgrade the client program so that we have the
	code that matches the monitor extension's version.

As a result, here is the standard upgrade plan for pg_auto_failover:

  1. Put all the current secondary nodes in maintenance to prevent from any
     failover to happen.

	 Run the follow SQL query on the monitor database to make sure your
	 setup current state allows for an upgrade operation to happen::

		select nodeid, nodename, nodeport, goalstate, reportedstate
		  from pgautofailover.node
		 where reportedstate = 'secondary'
		   and not exists
			  (
			   select nodeid
				 from pgautofailover.node
				where goalstate <> reportedstate
				   or reportedstate not in ('primary', 'secondary')
			  )
		order by nodeid;

	 The previous SQL query is expected to list all your Postgres nodes that
	 are currently in the `secondary` state. If the cluster is not in a
	 globally stable state, then this query returns no rows, and you should
	 stop the upgrade now.

	 A globally stable state is achieved when all the registered nodes are
	 either in the state `primary` or in the state `secondary`, and their
	 currently reported and goal state are the same.

	 Run the follow SQL query on the monitor database to enable the
	 maintenance operation::

		select nodeid, nodename, nodeport,
		       pgautofailover.start_maintenance(nodename, nodeport)
		  from pgautofailover.node
		 where reportedstate = 'secondary'
		   and not exists
			  (
			   select nodeid
				 from pgautofailover.node
				where goalstate <> reportedstate
				   or reportedstate not in ('primary', 'secondary')
			  )
		order by nodeid;

	 Check that the selected nodes have reached `maintenance` by watching
	 the output of the ``pg_autoctl show state`` command. When all selected
	 nodes are in maintenance, we can move forward with the upgrade.

	 .. note::

		Alternatively you may also run the command ``pg_autoctl enable
		maintenance`` on every secondary node.

  2. Stop the ``pg_autoctl`` service on the monitor.

	 The command to stop the systemd pgautofailover service is::

	   sudo systemctl stop pgautofailover

	 To verify that the service has been stopped as expected, we run the
	 following command::

	   sudo systemctl status pgautofailover

	 At this point all the Postgres nodes in the system are going to error
	 out every second when connecting to the monitor. In case of error to
	 connect to the monitor, the node-active process retries every second,
	 and will be able to continue normally as soon as the monitor is back
	 online.

  3. Upgrade the pg_auto_failover package on the monitor.

	 When using a debian based OS, this looks like the following command for
	 pg_auto_failover 1.3.1::

	   sudo apt-get remove pg-auto-failover-cli-enterprise-1.0 postgresql-11-auto-failover-enterprise-1.0
	   sudo apt-get install -q -y pg-auto-failover-cli-enterprise-1.3 postgresql-11-auto-failover-enterprise-1.3

  4. Restart the ``pgautofailover`` service on the monitor.

	 When using the systemd integration, all we need to do is::

	   sudo systemctl start pgautofailover

	 Then we may use the following commands to make sure that the service is
	 running as expected::

	   sudo systemctl status pgautofailover
	   sudo journalctl -u pgautofailover

	 At this point it is expected that the ``pg_autoctl`` logs show that an
	 upgrade has been performed by using the ``ALTER EXTENSION
	 pgautofailover UPDATE TO ...`` command. The monitor is ready with the
	 new version of pg_auto_failover.

  5. Upgrade the pg_auto_failover package on the Postgres nodes.

	 Use the same command as on the monitor in step 4 above.

  6. Restart the ``node-service`` service on all the Postgres nodes.

	 When using the systemd integration, restarting the ``pgautofailover``
	 service will also restart Postgres, which is not always possible. If
	 you did implement step 2, your current service is not running, so you
	 can restart it now::

	   pg_autoctl restart node-active

  7. Disable maintenance on the Postgres nodes.

	 For that we can run the following SQL query on the monitor database::

		select nodeid, nodename, nodeport,
		       pgautofailover.stop_maintenance(nodename, nodeport)
		  from pgautofailover.node
		 where reportedstate = 'maintenance';

	 .. note::

		Alternatively you may also run the command ``pg_autoctl disable
		maintenance`` on every secondary node.

And when the upgrade is done we can use ``pg_autoctl show state`` on the
monitor to see that eveything is as expected.

Cluster Management and Operations
---------------------------------

It is possible to operate pg_auto_failover formations and groups directly
from the monitor. All that is needed is an access to the monitor Postgres
database as a client, such as ``psql``. It's also possible to add those
management SQL function calls in your own ops application if you have one.

For security reasons, the ``autoctl_node`` is not allowed to perform
maintenance operations. This user is limited to what ``pg_autoctl`` needs.
You can either create a specific user and authentication rule to expose for
management, or edit the default HBA rules for the ``autoctl`` user. In the
following examples we're directly connecting as the ``autoctl`` role.

The main operations with pg_auto_failover are node maintenance and manual
failover, also known as a controlled switchover.

Maintenance of a secondary node
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

It is possible to put a secondary node in any group in a MAINTENANCE state,
so that the Postgres server is not doing *synchronous replication* anymore
and can be taken down for maintenance purposes, such as security kernel
upgrades or the like.

The monitor exposes the following API to schedule maintenance operations on
a secondary node::

  $ psql postgres://autoctl@monitor/pg_auto_failover
  > select pgautofailover.start_maintenance('nodename', 5432);
  > select pgautofailover.stop_maintenance('nodename', 5432);

The command line tool ``pg_autoctl`` also exposes an API to schedule
maintenance operations on the current node, which must be a secondary node
at the moment when maintenance is requested::

  $ pg_autoctl enable maintenance
  ...
  $ pg_autoctl disable maintenance

When a standby node is in maintenance, the monitor sets the primary node
replication to WAIT_PRIMARY: in this role, the PostgreSQL streaming
replication is now asynchronous and the standby PostgreSQL server may be
stopped, rebooted, etc.

pg_auto_failover does not provide support for primary server maintenance.

Triggering a failover
^^^^^^^^^^^^^^^^^^^^^

It is possible to trigger a failover manually with pg_auto_failover, by
using the SQL API provided by the monitor::

  $ psql postgres://autoctl@monitor/pg_auto_failover
  > select pgautofailover.perform_failover(formation_id => 'default', group_id => 0);

To call the function, you need to figure out the formation and group of the
group where the failover happens. The following commands when run on a
pg_auto_failover keeper node provide for the necessary information::

  $ export PGDATA=...
  $ pg_autoctl config get pg_autoctl.formation
  $ pg_autoctl config get pg_autoctl.group

Implementing a controlled switchover
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

It is generally useful to distinguish a *controlled switchover* to a
*failover*. In a controlled switchover situation it is possible to organise
the sequence of events in a way to avoid data loss and lower downtime to a
minimum.

In the case of pg_auto_failover, because we use **synchronous replication**,
we don't face data loss risks when triggering a manual failover. Moreover,
our monitor knows the current primary health at the time when the failover
is triggerred, and drives the failover accordingly.

So to trigger a controlled switchover with pg_auto_failover you can use the
same API as for a manual failover::

  $ psql postgres://autoctl@monitor/pg_auto_failover
  > select pgautofailover.perform_failover(formation_id => 'default', group_id => 0);

Current state, last events
--------------------------

The following commands display information from the pg_auto_failover monitor tables
``pgautofailover.node`` and ``pgautofailover.event``:

::

  $ pg_autoctl show state
  $ pg_autoctl show events

When run on the monitor, the commands outputs all the known states and
events for the whole set of formations handled by the monitor. When run on a
PostgreSQL node, the command connects to the monitor and outputs the
information relevant to the service group of the local node only.

For interactive debugging it is helpful to run the following command from
the monitor node while e.g. initializing a formation from scratch, or
performing a manual failover::

  $ watch pg_autoctl show state

Monitoring pg_auto_failover in Production
-----------------------------------------

The monitor reports every state change decision to a LISTEN/NOTIFY channel
named ``state``. PostgreSQL logs on the monitor are also stored in a table,
``pgautofailover.event``, and broadcast by NOTIFY in the channel ``log``.

Trouble-Shooting Guide
----------------------

pg_auto_failover commands can be run repeatedly. If initialization fails the first
time -- for instance because a firewall rule hasn't yet activated -- it's
possible to try ``pg_autoctl create`` again. pg_auto_failover will review its previous
progress and repeat idempotent operations (``create database``, ``create
extension`` etc), gracefully handling errors.
