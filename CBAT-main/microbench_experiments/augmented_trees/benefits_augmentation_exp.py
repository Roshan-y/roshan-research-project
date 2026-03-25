from _basic_functions import *
import pandas
import matplotlib as mpl

def plot_func(filename, column_filters, data, series_name, x_name, y_name, exp_dict=None):
    table_base = pandas.pivot_table(data, index=x_name, columns=series_name, values=y_name, aggfunc=get_average)
    # table_base.drop(table_base.tail(1).index,inplace=True)
    table = table_base.rename(columns=series_name_map)
    ax = table.plot(kind='line', logy=True, logx=True, color=['tab:blue', 'tab:orange', 'tab:green', 'tab:purple', 'tab:red'], style=['o-', 's-', '^-', 'v-', 'D-'], ylabel='Throughput')
    ax.set_xlabel("Range Query Size", fontsize=14)
    ax.set_ylabel("Throughput", fontsize=14)
    ax.tick_params(axis='both', which='major', labelsize=12)
    ax.tick_params(axis='both', which='minor', labelsize=12)
    handles, labels = ax.get_legend_handles_labels()
    order = [2, 0, 1, 4, 3]
    reordered_handles = [handles[i] for i in order]
    reordered_labels = [labels[i] for i in order]
    ax.legend(reordered_handles, reordered_labels, fontsize=11)
    t = ax.yaxis.get_offset_text()
    t.set_size(12)
    mpl.pyplot.savefig(filename)
    print('## SAVED FIGURE {}'.format(filename))

def define_experiment(exp_dict, args):
    set_dir_tools    (exp_dict, os.getcwd() + '/../../tools')
    set_dir_compile  (exp_dict, os.getcwd() + '/../../microbench')
    set_dir_run      (exp_dict, os.getcwd() + '/../../microbench')
    set_cmd_compile  (exp_dict, 'make -j ds-reclaim has_libpapi=0')
    set_dir_data     (exp_dict, os.getcwd() + '/data_augmentation')

    add_run_param    (exp_dict, 'TOTAL_THREADS', [min(120, int(sys.argv[1]))])
    add_run_param    (exp_dict, '__trials', [1, 2, 3, 4, 5])
    add_run_param    (exp_dict, 'DS_TYPENAME', ['brown_ext_chromatic_delegateSingle_lf', 'wei_ext_vcas_bst_lf', 'arbel_int_bst_lf', 'blelloch_ext_btree_lf', 'ellen_augmented_ext_bst_lf'])
    add_run_param    (exp_dict, 'MAXKEY', [100000, 10000000]) 
    add_run_param    (exp_dict, 'RQSIZE', [8, 100, 2000, 50000])
    add_run_param    (exp_dict, 'ZIPF_PARAM', ['0'])
    add_run_param    (exp_dict, 'INS_DEL_FRAC', ['10.0 10.0'])
    add_data_field   (exp_dict, 'total_throughput', coltype='INTEGER')

    # threads = min(60, int(sys.argv[1])/2)
    set_cmd_run      (exp_dict, 'LD_PRELOAD=../lib/libmimalloc.so ./bin/{DS_TYPENAME}.debra -nwork {TOTAL_THREADS} -nprefill 30 -insdel {INS_DEL_FRAC} -dist-zipf {ZIPF_PARAM} -autorq -rqsize {RQSIZE} -k {MAXKEY} -t 3000')
    
    # Benefits of Augmentation
    add_plot_set(
            exp_dict
          , name='augmentation_maxkey={MAXKEY}.pdf'
          , title='Range Query Size vs Throughput'
          , varying_cols_list=['MAXKEY']
          , series='DS_TYPENAME'
          , x_axis='RQSIZE'
          , y_axis='total_throughput'
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