#!/bin/sh
#
# Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.
#

# set $? to zero iff verbose mode is on
is_verbose()
{
	[ ${verbose} -ne 0 ]
}

# Show a message in verbose mode only
verbose_log()
{
	is_verbose && echo "$@"
}

# Show error message and exit
error()
{
	echo
	echo -e "Error: $@"
	echo
	exit -2
}

# Show the value of a variable
show_var()
{
	value=$(eval echo \$$1)
	printf "    %20s : '%s'\n" $1 "${value}"
}

# Show the value of a variable only if verbose
show_var_verbose()
{
	is_verbose && show_var $1
}

# Split a comma-separated list to space-separated list which can be used by 'for'
split_list()
{
	echo $1 | tr ',' ' '
}

# return minimal number of two integer arguments
min()
{
	[ $1 -lt $2 ] && echo $1 || echo $2
}

# Update some default configuration settings according to slurm launch
check_slurm_env()
{
	if [ -z "$SLURM_JOBID" ]
	then
		# Skip slurm
		return
	fi

	# Nodes to run on
	host_list=$(hostlist -s, -e $(squeue -j ${SLURM_JOBID} -h -o "%N"))
	SLURM_NNODES=$(squeue -j ${SLURM_JOBID} -h -o "%D")
	NNODES=$SLURM_NNODES

	if [ -n "$SLURM_TASKS_PER_NODE" ]
	then
		tasks_per_node=$(echo $SLURM_TASKS_PER_NODE | cut -d'(' -f1)
	elif [ -n "$SLURM_JOB_CPUS_PER_NODE" ]
	then
		tasks_per_node=$(echo $SLURM_JOB_CPUS_PER_NODE | cut -d'(' -f1)
	else
		TOTAL_CPUS=$(squeue -j ${SLURM_JOBID} -h -o "%C")
		tasks_per_node=$((${TOTAL_CPUS} / ${SLURM_NNODES}))
	fi
}

# Set default configuration values
init_config()
{
	verbose=0
	iodemo_exe=""
	iodemo_client_args=""
	num_clients=1
	num_servers=1
	map_by="node"
	base_port_num=20000
	tasks_per_node=1
	net_if="bond0"
	duration=30
	client_wait_time=2
	launcher="pdsh -b -w"
	dry_run=0

	# command line args will override slurm env vars
	check_slurm_env
}

# Show the configuration
show_config()
{
	echo "Launch configuration:"
	for key in \
			host_list tasks_per_node map_by \
			num_clients num_servers \
			iodemo_exe iodemo_client_args \
			net_if base_port_num \
			duration client_wait_time \
			launcher dry_run
	do
		show_var ${key}
	done
}

# Helper to show default value if defined, otherwise empty string
show_default_value()
{
	value=$(eval echo \$$1)
	[ -z "${value}" ] && echo "" || echo " (${value})"
}

# Show script usage help message
usage()
{
	echo "Usage: run_io_demo.sh <options> <exe> <arguments>"
	echo
	echo "  -h|--help                   Show this help message"
	echo "  -v|--verbose                Turn on verbosity"
	echo "  -H|--hostlist <h1>,<h2>,..  List of host names to run on"$(show_default_value host_list)
	echo "  -i|--netif <n>              Network interface to use"$(show_default_value net_if)
	echo "  -d|--duration <seconds>     How much time to run the application"$(show_default_value duration)
	echo "  --num-clients <count>       Number of clients to run"$(show_default_value num_clients)
	echo "  --num-servers <count>       Number of servers to run"$(show_default_value num_servers)
	echo "  --tasks-per-node <count>    Maximal number of tasks per node"$(show_default_value tasks_per_node)
	echo "  --map-by <mode>             Mapping mode of processes to hosts"$(show_default_value map_by)
	echo "                               - node : Distribute clients and servers equally"
	echo "                               - slot : Place the servers first, then the clients"
	echo "  --client-wait <seconds>     Time the client sleeps before starting"$(show_default_value client_wait_time)
	echo "  --launcher <command>        Command to start distributed run"$(show_default_value launcher)
	echo "                                The syntax of launcher command should be:"
	echo "                                <command> host1,host2,... <exe> <args>"
	echo "  --dry-run                   Do not launch the application, just generate run scripts"
	echo
}

# Parse command line arguments and update configuration variables
parse_args()
{
	while [[ $# -gt 0 ]]
	do
		key="$1"
		case $key in
		-h|--help)
			usage
			exit 0
			;;
		-v|--verbose)
			verbose=1
			;;
		-H|--hostlist)
			host_list="$2"
			shift
			;;
		-i|--netif)
			net_if="$2"
			shift
			;;
		-d|--duration)
			duration="$2"
			shift
			;;
		--num-clients)
			num_clients="$2"
			shift
			;;
		--num-servers)
			num_servers="$2"
			shift
			;;
		--tasks-per-node)
			tasks_per_node="$2"
			shift
			;;
		--map-by)
			map_by="$2"
			shift
			;;
		--client-wait)
			client_wait_time="$2"
			shift
			;;
		--launcher)
			launcher="$2"
			shift
			;;
		--dry-run)
			dry_run=1
			;;
		[^-]*)
			iodemo_exe="$key"
			shift
			break
			;;
		*)
			usage
			error "Invalid parameter '${key}'"
			;;
		esac
		shift
	done

	iodemo_client_args="$@"
}

check_params()
{
	if [ -z ${iodemo_exe} ]
	then
		error "missing executable from command line"
	fi
}

# set SSH options for different launchers
set_ssh_options()
{
	ssh_flags="-q"
	ssh_flags+=" -oStrictHostKeyChecking=no"
	ssh_flags+=" -oUserKnownHostsFile=/dev/null"
	ssh_flags+=" -oPreferredAuthentications=publickey"
	show_var_verbose ssh_flags

	launcher_exe=$(echo ${launcher} | awk '{print $1}')
	show_var_verbose launcher_exe

	case $(basename ${launcher_exe}) in
	pdsh*)
		# pdsh accepts options from environment variable
		export PDSH_SSH_ARGS_APPEND="${ssh_flags}"
		show_var_verbose PDSH_SSH_ARGS_APPEND
		;;
	clush*)
		# clush accepts options list by '-o <list>', so insert ssh options list
		# right after executable path
		launcher=$(echo ${launcher} | sed -e "s!^\(${launcher_exe}\)!\1 -o \"${ssh_flags}\"!")
		show_var_verbose launcher
		;;
	*)
		echo "Warning: unknown launcher '${launcher}', not setting SSH options"
		;;
	esac
}

collect_ip_addrs()
{
	# convert the output of 'ip' to 'host:ip' list
	host_ips=$(eval ${launcher} ${host_list} ip -4 -o address show ${net_if} |
			   sed -ne 's/^\(\S*\): .* inet \([0-9\.]*\).*$/\1:\2/p')
	if [ $(echo ${host_ips} | wc -w) -ne $(split_list ${host_list} | wc -w) ]
	then
		error "failed to collect host IP addresses for ${net_if}"
	fi

	# map the ips to hosts according to host order in the list
	for host_ip in ${host_ips}
	do
		host=$(echo ${host_ip} | cut -d: -f1)
		addr=$(echo ${host_ip} | cut -d: -f2)
		if [ -n ${host} ] && [ -n ${addr} ]
		then
			ip_address_per_host[${host}]=${addr}
		fi
	done
}

build_server_args_list() {
	iodemo_server_args=""
	while [[ $# -gt 0 ]]
	do
		key="$1"
		case $key in
		-d)
			value="$2"
			iodemo_server_args+=" $key $value"
			shift
			;;
		*)
			;;
		esac
		shift
	done

	show_var_verbose iodemo_server_args
}

make_scripts()
{
	# save number of hosts
	num_hosts=$(split_list ${host_list} | wc -w)
	if [ ${num_hosts} -eq 0 ]
	then
		error "no hosts specified"
	fi

	show_var_verbose num_hosts

	# build server-side arguments based on client arguments
	build_server_args_list ${iodemo_client_args}

	# initialize mapping-related variables
	case ${map_by} in
	node)
		max_clients_per_node=$(( (num_clients + num_hosts - 1) / num_hosts ))
		max_servers_per_node=$(( (num_servers + num_hosts - 1) / num_hosts ))
		max_ppn_per_node=$((max_clients_per_node + max_servers_per_node))
		if [ ${max_ppn_per_node} -gt ${tasks_per_node} ]
		then
			error "trying to launch ${max_clients_per_node} clients and" \
			      "${max_servers_per_node} servers per node, but only" \
			      "${tasks_per_node} tasks are allowed." \
			      "\nPlease increase --tasks-per-node parameter or use more nodes."
		fi
		;;
	slot)
		num_tasks=$((num_clients + num_servers))
		max_tasks=$((tasks_per_node * num_hosts))
		if [ ${num_tasks} -gt ${max_tasks} ]
		then
			error "trying to launch ${num_clients} clients and" \
			      "${num_servers} servers, but only ${max_tasks} tasks" \
			      "in total are allowed." \
			      "\nPlease increase --tasks-per-node parameter or use more nodes."
		fi
		;;
	*)
		error "invalid mapping type '${map_by}'"
		;;
	esac

	# collect ip addresses of each host
	declare -A ip_address_per_host
	collect_ip_addrs

	#
	# create the process mapping: fill 'num_servers_per_host' and
	# 'num_clients_per_host' associative arrays, and build 'client_connect_list'
	#
	declare -A num_servers_per_host
	declare -A num_clients_per_host
	client_connect_list=""
	added_servers=0
	added_clients=0
	for host in $(split_list ${host_list})
	do
		case ${map_by} in
		node)
			# For each node, add both servers and clients, the last node may
			# have less than others
			node_num_servers=$(min ${max_servers_per_node} $((num_servers - added_servers)))
			node_num_clients=$(min ${max_clients_per_node} $((num_clients - added_clients)))
			;;
		slot)
			# Servers take slots first, and clients take what is left
			node_num_servers=$(min ${tasks_per_node} $((num_servers - added_servers)))
			node_num_clients=$(min $((tasks_per_node - node_num_servers)) \
			                       $((num_clients - added_clients)))
			;;
		esac

		num_servers_per_host[${host}]=${node_num_servers}
		num_clients_per_host[${host}]=${node_num_clients}

		for ((i=0;i<node_num_servers;++i))
		do
			port_num=$((base_port_num + i))
			client_connect_list+=" ${ip_address_per_host[${host}]}:${port_num}"
		done

		added_servers=$((added_servers + node_num_servers))
		added_clients=$((added_clients + node_num_clients))
	done

	#
	# In verbose mode, make following changes:
	#   1. show mixed output in stdout (in addition to log files)
	#   2. run the spawned scripts with '-x'
	#   3. show output from kill commands
	#
	if is_verbose
	then
		show_var client_connect_list
		sh_args="-x"
		wait_redirect=""
		log_redirect="|& tee "
	else
		sh_args=""
		wait_redirect=">& /dev/null"
		log_redirect=">& "
	fi

	exe_basename=$(basename ${iodemo_exe})

	# create run scripts
	for host in $(split_list ${host_list})
	do
		command_file="iodemo_commands_${host}.sh"

		# Add file header and startup
		cat >${command_file} <<-EOF
			#!/bin/sh ${sh_args}
			#
			# Launch script for io_demo on ${host} with ${num_servers_per_host[${host}]} servers and ${num_clients_per_host[${host}]} clients
			#

			kill_iodemo() {
			    # kill process by exact name to avoid killing launch scirpts
			    # lt- prefix is in case iodemo is wrapped by libtool
			    for pattern in ${exe_basename} lt-${exe_basename}
			    do
			        pkill --exact -u ${USER} -9 \${pattern}
			    done
			}

			signal_handler() {
			    echo "Got signal, killing all iodemo processes"
			    kill_iodemo
			}

			# kill existing processes and trap signals
			trap signal_handler INT TERM
			kill_iodemo

			# Use same work dir as launch script
			cd $PWD

			# UCX inherited environment variables
			EOF

		# Add all relevant env vars which start with UCX_ to the command file
		env | grep -P '^UCX_.*=|^PATH=|^LD_LIBRARY_PATH=' | \
			xargs -L 1 echo export >>${command_file}

		# Add servers commands
		cat >>${command_file} <<-EOF

			# Server commands
			EOF

		cmd_prefix="stdbuf -e0 -o0 timeout -s 9 $((duration + client_wait_time))s"
		for ((i=0;i<${num_servers_per_host[${host}]};++i))
		do
			port_num=$((base_port_num + i))
			log_file=$(printf "iodemo_%s_server_%02d.log" ${host} $i)
			echo ${log_file}
			cat >>${command_file} <<-EOF
				${cmd_prefix} \\
				    ${iodemo_exe} \\
				        ${iodemo_server_args} -p ${port_num} \\
				        ${log_redirect} ${log_file} &
				EOF
		done

		# Add short sleep to let servers start running
		cat >>${command_file} <<-EOF

			# Wait for servers to start
			sleep ${client_wait_time}
			EOF

		# Add client commands
		cat >>${command_file} <<-EOF

			# Client commands
			EOF

		cmd_prefix="stdbuf -e0 -o0 timeout -s 9 ${duration}s"
		for ((i=0;i<num_clients_per_host[${host}];++i))
		do
			log_file=$(printf "iodemo_%s_client_%02d.log" ${host} $i)
			echo ${log_file}
			cat >>${command_file} <<-EOF
				${cmd_prefix} \\
				    ${iodemo_exe} \\
				        ${iodemo_client_args} ${client_connect_list} \\
				        ${log_redirect} ${log_file} &
				EOF
		done

		# Add wait commands
		cat >>${command_file} <<-EOF

			# Wait for background processes
			wait ${wait_redirect}
			echo "Test finished"
			EOF
		chmod a+x ${command_file}
	done
}

kill_all() {
	# signal handler to kill spawned processes
	echo "Interrupt: killing running processes"
	exe_basename=$(basename ${iodemo_exe})
	kill_command="pkill -u ${USER} --exact"
	eval ${launcher} ${host_list} \
		"${kill_command} ${exe_basename}; ${kill_command} lt-${exe_basename};"
}

run() {
	if [ ${dry_run} -eq 1 ]
	then
		for host in $(split_list ${host_list})
		do
			command_file="$PWD/iodemo_commands_${host}.sh"
			echo "${host} : ${command_file}"
			if is_verbose
			then
				awk '{printf "\t%3d.\t%s\n", NR, $0}' < ${command_file}
			fi
		done
	else
		trap kill_all SIGINT
		eval ${launcher} ${host_list} "$PWD/iodemo_commands_'\$(hostname -s)'.sh"
	fi
}

main()
{
	init_config
	parse_args "$@"
	check_params
	show_config
	set_ssh_options
	make_scripts
	run
}

main "$@"