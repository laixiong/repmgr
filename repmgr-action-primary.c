/*
 * repmgr-action-primary.c
 *
 * Implements primary actions for the repmgr command line utility
 *
 * Copyright (c) 2ndQuadrant, 2010-2017
 */

#include "repmgr.h"

#include "repmgr-client-global.h"
#include "repmgr-action-primary.h"


/*
 * do_primary_register()
 *
 * Event(s):
 *  - primary_register
 */
void
do_primary_register(void)
{
	PGconn	   *conn = NULL;
	PGconn	   *primary_conn = NULL;
	int			current_primary_id = UNKNOWN_NODE_ID;
	RecoveryType recovery_type;
	t_node_info node_info = T_NODE_INFO_INITIALIZER;
	RecordStatus record_status;

	bool		record_created;

	PQExpBufferData	  event_description;

	log_info(_("connecting to primary database..."));

	conn = establish_db_connection(config_file_options.conninfo, true);
	log_verbose(LOG_INFO, _("connected to server, checking its state"));

	/* verify that node is running a supported server version */
	check_server_version(conn, "primary", true, NULL);

	/* check that node is actually a primary */
	recovery_type = get_recovery_type(conn);

	if (recovery_type != RECTYPE_PRIMARY)
	{
		if (recovery_type == RECTYPE_STANDBY)
		{
			log_error(_("server is in standby mode and cannot be registered as a primary"));
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}
		else
		{
			log_error(_("connection to node lost"));
			PQfinish(conn);
			exit(ERR_DB_CONN);
		}
	}

	log_verbose(LOG_INFO, _("server is not in recovery"));

	/* create the repmgr extension if it doesn't already exist */
	if (!create_repmgr_extension(conn))
	{
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * In --dry-run mode we can't proceed any further as the following code
	 * attempts to query the repmgr metadata, which won't exist until
	 * the extension is installed
	 */
	if (runtime_options.dry_run == true)
	{
		PQfinish(conn);
		return;
	}


	/* Ensure there isn't another registered node which is primary */
	primary_conn = get_primary_connection(conn, &current_primary_id, NULL);

	if (primary_conn != NULL)
	{
		if (current_primary_id != config_file_options.node_id)
		{
			/* it's impossible to add a second primary to a streaming replication cluster */
			log_error(_("there is already an active registered primary (node ID: %i) in this cluster"), current_primary_id);
			PQfinish(primary_conn);
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		/* we've probably connected to ourselves */
		PQfinish(primary_conn);
	}


	begin_transaction(conn);

	/*
	 * Check for an active primary node record with a different ID. This shouldn't
	 * happen, but could do if an existing primary was shut down without being unregistered.
	*/
	current_primary_id = get_primary_node_id(conn);
	if (current_primary_id != NODE_NOT_FOUND && current_primary_id != config_file_options.node_id)
	{
		log_error(_("another node with id %i is already registered as primary"), current_primary_id);
		log_detail(_("a streaming replication cluster can have only one primary node"));

		rollback_transaction(conn);
		PQfinish(conn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * Check whether there's an existing record for this node, and
	 * update it if --force set
	 */

	record_status = get_node_record(conn, config_file_options.node_id, &node_info);

	if (record_status == RECORD_FOUND)
	{
		if (!runtime_options.force)
		{
			log_error(_("this node is already registered"));
			log_hint(_("use -F/--force to overwrite the existing node record"));
			rollback_transaction(conn);
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}
	}
	else
	{
		node_info.node_id = config_file_options.node_id;
	}

	/* set type to "primary", active to "true" and unset upstream_node_id*/
	node_info.type = PRIMARY;
	node_info.upstream_node_id = NO_UPSTREAM_NODE;
	node_info.active = true;

	/* update node record structure with settings from config file */
	strncpy(node_info.node_name, config_file_options.node_name, MAXLEN);
	strncpy(node_info.conninfo, config_file_options.conninfo, MAXLEN);

	if (config_file_options.replication_user[0] != '\0')
	{
		strncpy(node_info.repluser, config_file_options.replication_user, NAMEDATALEN);
	}
	else
	{
		(void)get_conninfo_value(config_file_options.conninfo, "user", node_info.repluser);
	}

	if (repmgr_slot_name_ptr != NULL)
		strncpy(node_info.slot_name, repmgr_slot_name_ptr, MAXLEN);

	strncpy(node_info.location, config_file_options.location, MAXLEN);
	node_info.priority = config_file_options.priority;

	initPQExpBuffer(&event_description);

	if (record_status == RECORD_FOUND)
	{
		record_created = update_node_record(conn,
											"primary register",
											&node_info);
		if (record_created == true)
		{
			appendPQExpBuffer(&event_description,
							  "existing primary record updated");
		}
		else
		{
			appendPQExpBuffer(&event_description,
							  "error encountered while updating primary record:\n%s",
							  PQerrorMessage(conn));
		}

	}
	else
	{
		record_created = create_node_record(conn,
											"primary register",
											&node_info);
		if (record_created == false)
		{
			appendPQExpBuffer(&event_description,
							  "error encountered while creating primary record:\n%s",
							  PQerrorMessage(conn));
		}

	}

	if (record_created == false)
	{
		rollback_transaction(conn);
	}
	else
	{
		commit_transaction(conn);
	}

	/* Log the event */
	create_event_notification(conn,
						&config_file_options,
						config_file_options.node_id,
						"primary_register",
						record_created,
						event_description.data);


	PQfinish(conn);

	if (record_created == false)
	{
		log_notice(_("unable to register primary node - see preceding messages"));
		exit(ERR_DB_QUERY);
	}

	if (record_status == RECORD_FOUND)
	{
		log_notice(_("primary node record (id: %i) updated"),
				   config_file_options.node_id);
	}
	else
	{
		log_notice(_("primary node record (id: %i) registered"),
				   config_file_options.node_id);
	}

	return;
}


/*
 * do_primary_unregister()
 *
 * Event(s):
 *  - primary_unregister
 */

void
do_primary_unregister(void)
{
	PGconn	    *primary_conn = NULL;
	PGconn	    *local_conn = NULL;
	t_node_info  local_node_info = T_NODE_INFO_INITIALIZER;

	t_node_info *target_node_info_ptr;
	PGconn	    *target_node_conn = NULL;

	NodeInfoList downstream_nodes = T_NODE_INFO_LIST_INITIALIZER;

	/* We must be able to connect to the local node */
	local_conn = establish_db_connection(config_file_options.conninfo, true);

	/* Get local node record */
	get_local_node_record(local_conn, config_file_options.node_id, &local_node_info);

	/*
	 * Obtain a connection to the current primary node - if this isn't possible,
	 * abort as we won't be able to update the "nodes" table anyway.
	 */
	primary_conn = establish_primary_db_connection(local_conn, false);

	if (PQstatus(primary_conn) != CONNECTION_OK)
	{
		t_node_info primary_node_info;

		log_error(_("unable to connect to primary server"));

		if (get_primary_node_record(local_conn, &primary_node_info))
		{
			log_detail(_("current primary registered as node %s (id: %i, conninfo: \"%s\")"),
					   primary_node_info.node_name,
					   primary_node_info.node_id,
					   primary_node_info.conninfo);
		}

		log_hint(_("you may need to promote this standby or ask it to look for a new primary to follow"));
		PQfinish(local_conn);
		exit(ERR_DB_CONN);
	}

	/* Local connection no longer required */
	PQfinish(local_conn);


	/* Target node is local node? */
	if (target_node_info.node_id == UNKNOWN_NODE_ID
	 || target_node_info.node_id == config_file_options.node_id)
	{
		target_node_info_ptr = &local_node_info;
	}
	/* Target node is explicitly specified, and is not local node */
	else
	{
		target_node_info_ptr = &target_node_info;
	}

	/*
	 * Check for downstream nodes - if any still defined, we won't be able to
	 * delete the node record due to foreign key constraints.
	 */
	get_downstream_node_records(primary_conn, target_node_info_ptr->node_id, &downstream_nodes);

	if (downstream_nodes.node_count > 0)
	{
		NodeInfoListCell *cell;
		PQExpBufferData   detail;

		if (downstream_nodes.node_count == 1)
		{
			log_error(_("%i other node still has this node as its upstream node"),
					  downstream_nodes.node_count);
		}
		else
		{
			log_error(_("%i other nodes still have this node as their upstream node"),
					  downstream_nodes.node_count);
		}

		log_hint(_("ensure these nodes are following the current primary with \"repmgr standby follow\""));

		initPQExpBuffer(&detail);

		for (cell = downstream_nodes.head; cell; cell = cell->next)
		{
			appendPQExpBuffer(&detail,
							  "  %s (id: %i)\n",
							  cell->node_info->node_name,
							  cell->node_info->node_id);
		}

		log_detail(_("the affected node(s) are:\n%s"), detail.data);

		termPQExpBuffer(&detail);
		PQfinish(primary_conn);

		exit(ERR_BAD_CONFIG);
	}

	target_node_conn = establish_db_connection_quiet(target_node_info_ptr->conninfo);

	/* If node not reachable, check that the record is for a primary node */
	if (PQstatus(target_node_conn) != CONNECTION_OK)
	{
		if (target_node_info_ptr->type != PRIMARY)
		{
			log_error(_("node %s (id: %i) is not a primary, unable to unregister"),
					  target_node_info_ptr->node_name,
					  target_node_info_ptr->node_id);
			if (target_node_info_ptr->type == STANDBY)
			{
				log_hint(_("the node can be unregistered with \"repmgr standby unregister\""));
			}

			PQfinish(primary_conn);
			exit(ERR_BAD_CONFIG);
		}
	}
	/* If we can connect to the node, perform some sanity checks on it */
	else
	{
		bool can_unregister = true;
		RecoveryType recovery_type = get_recovery_type(target_node_conn);

		/* Node appears to be a standby */
		if (recovery_type == RECTYPE_STANDBY)
		{
			/* We'll refuse to do anything unless the node record shows it as a primary */

			if (target_node_info_ptr->type != PRIMARY)
			{
				log_error(_("node %s (id: %i) is a %s, unable to unregister"),
						  target_node_info_ptr->node_name,
						  target_node_info_ptr->node_id,
						  get_node_type_string(target_node_info_ptr->type));
				can_unregister = false;
			}
			/*
			 * If --F/--force not set, hint that it might be appropriate to
			 * register the node as a standby rather than unregister as primary
			 */
			else if (!runtime_options.force)
			{
				log_error(_("node %s (id: %i) is running as a standby, unable to unregister"),
						  target_node_info_ptr->node_name,
						  target_node_info_ptr->node_id);
				log_hint(_("the node can be registered as a standby with \"repmgr standby register --force\""));
				log_hint(_("use \"repmgr primary unregister --force\" to remove this node's metadata entirely"));
				can_unregister = false;
			}


			if (can_unregister == false)
			{
				PQfinish(target_node_conn);
				PQfinish(primary_conn);
				exit(ERR_BAD_CONFIG);
			}
		}
		else if (recovery_type == RECTYPE_PRIMARY)
		{
			t_node_info  primary_node_info = T_NODE_INFO_INITIALIZER;
			bool primary_record_found;

			primary_record_found = get_primary_node_record(primary_conn, &primary_node_info);

			if (primary_record_found == false)
			{
				log_error(_("node %s (id: %i) is a primary node, but no primary node record found"),
						  target_node_info_ptr->node_name,
						  target_node_info_ptr->node_id);
				log_hint(_("register this node as primary with \"repmgr primary register --force\""));
				PQfinish(target_node_conn);
				PQfinish(primary_conn);
				exit(ERR_BAD_CONFIG);
			}
			/* This appears to be the cluster primary - cowardly refuse
			 * to delete the record
			 */
			if (primary_node_info.node_id == target_node_info_ptr->node_id)
			{
				log_error(_("node %s (id: %i) is the current primary node, unable to unregister"),
						  target_node_info_ptr->node_name,
						  target_node_info_ptr->node_id);

				if (primary_node_info.active == false)
				{
					log_hint(_("node is marked as inactive, activate with \"repmgr primary register --force\""));
				}
				PQfinish(target_node_conn);
				PQfinish(primary_conn);
				exit(ERR_BAD_CONFIG);
			}
		}

		/* We don't need the target node connection any more */
		PQfinish(target_node_conn);
	}

	if (target_node_info_ptr->active == true)
	{
		if (!runtime_options.force)
		{
			log_error(_("node %s (id: %i) is marked as active, unable to unregister"),
					  target_node_info_ptr->node_name,
					  target_node_info_ptr->node_id);
			log_hint(_("run \"repmgr primary unregister --force\" to unregister this node"));
			PQfinish(primary_conn);
			exit(ERR_BAD_CONFIG);
		}
	}

	if (runtime_options.dry_run == true)
	{
		log_notice(_("node %s (id: %i) would now be unregistered"),
				   target_node_info_ptr->node_name,
				   target_node_info_ptr->node_id);
		log_hint(_("run the same command without the --dry-run option to unregister this node"));
	}
	else
	{
		PQExpBufferData event_details;
		bool delete_success = delete_node_record(primary_conn,
												 target_node_info_ptr->node_id);

		if (delete_success == false)
		{
			log_error(_("unable to unregister node %s (id: %i)"),
					  target_node_info_ptr->node_name,
					  target_node_info_ptr->node_id);
			PQfinish(primary_conn);
			exit(ERR_DB_QUERY);
		}

		initPQExpBuffer(&event_details);
		appendPQExpBuffer(&event_details,
						  _("node %s (id: %i) unregistered"),
						  target_node_info_ptr->node_name,
						  target_node_info_ptr->node_id);

		if (target_node_info_ptr->node_id != config_file_options.node_id)
		{
			appendPQExpBuffer(&event_details,
							  _(" from node %s (id: %i)"),
							  config_file_options.node_name,
							  config_file_options.node_id);
		}

		create_event_notification(primary_conn,
							&config_file_options,
							config_file_options.node_id,
							"primary_unregister",
							true,
							event_details.data);
		termPQExpBuffer(&event_details);

		log_info(_("node %s (id: %i) was successfully unregistered"),
				 target_node_info_ptr->node_name,
				 target_node_info_ptr->node_id);
	}

	PQfinish(primary_conn);
	return;
}