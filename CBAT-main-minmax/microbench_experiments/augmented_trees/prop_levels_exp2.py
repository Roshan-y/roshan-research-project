from _basic_functions import *
import pandas
import matplotlib as mpl

def plot_func(filename, column_filters, data, series_name, x_name, y_name, exp_dict=None):
    table_base = pandas.pivot_table(data, index=x_name, columns=series_name, values=y_name, aggfunc=get_average)
    table = table_base.rename(columns=series_name_map)
    ax = table.plot(kind='line', style=['s-', '^-', 'o-'], ylabel='Avg Levels Propagated')
    ax.set_xlabel("Number of Threads", fontsize=14)
    ax.set_ylabel("Avg Levels Propagated", fontsize=14)
    ax.tick_params(axis='both', which='major', labelsize=12)
    ax.tick_params(axis='both', which='minor', labelsize=12)
    ax.legend(fontsize=11)
    t = ax.yaxis.get_offset_text()
    t.set_size(12)
    mpl.pyplot.savefig(filename)
    print('## SAVED FIGURE {}'.format(filename))

def define_experiment(exp_dict, args):
    set_dir_tools    (exp_dict, os.getcwd() + '/../../tools')
    set_dir_compile  (exp_dict, os.getcwd() + '/../../microbench')
    set_dir_run      (exp_dict, os.getcwd() + '/../../microbench')
    set_cmd_compile  (exp_dict, 'make -j ds-reclaim has_libpapi=0')
    set_dir_data     (exp_dict, os.getcwd() + '/data_propagate')

    mt = min(180, int(sys.argv[1]))
    add_run_param    (exp_dict, 'TOTAL_THREADS', [1, int(mt/6), int(mt/3), int(mt/2), int(2*mt/3), int(5*mt/6), mt])
    add_run_param    (exp_dict, '__trials', [1, 2, 3, 4, 5])
    add_run_param    (exp_dict, 'DS_TYPENAME', ['brown_ext_chromatic_augment_lf', 'brown_ext_chromatic_delegateDouble_lf', 'brown_ext_chromatic_delegateSingle_lf'])
    add_run_param    (exp_dict, 'MAXKEY', [10000000]) 
    add_run_param    (exp_dict, 'RQSIZE', [50000])
    add_run_param    (exp_dict, 'ZIPF_PARAM', ['0'])
    add_run_param    (exp_dict, 'INS_DEL_FRAC', ['50.0 50.0'])
    add_data_field   (exp_dict, 'Avg_nodes_per_propagate', coltype='REAL')

    set_cmd_run      (exp_dict, 'LD_PRELOAD=../lib/libmimalloc.so ./bin/{DS_TYPENAME}.debra -nwork {TOTAL_THREADS} -nprefill 30 -insdel {INS_DEL_FRAC} -dist-zipf {ZIPF_PARAM} -autorq -rqsize {RQSIZE} -k {MAXKEY} -t 3000')
    
    # Propagate Levels
    add_plot_set(
            exp_dict
          , name='prop_levels.pdf'
          , title='Total Threads vs Average Nodes Seen per Propagate'
          , varying_cols_list=[]
          , series='DS_TYPENAME'
          , x_axis='TOTAL_THREADS'
          , y_axis='Avg_nodes_per_propagate'
          , plot_type=plot_func
    )

import sys ; sys.path.append('../../tools/data_framework') ; from run_experiment import *
if (len(sys.argv) != 2):
    print("Need exactly one command line argument for maximum number of threads")
    sys.exit(0)
run_in_jupyter(define_experiment, cmdline_args='-crdp')
# init_for_jupyter('basic.py')

# df = select_to_dataframe('select * from data')
# display(df)