from _basic_functions import *
import pandas

def plot_func(filename, column_filters, data, series_name, x_name, y_name, exp_dict=None):
    for row in data.itertuples(index=False):
        if(row.total_throughput > 0):
            print(series_name_map[row.DS_TYPENAME] + " Check Passed!")
        else:
            print(series_name_map[row.DS_TYPENAME] + " Check Failed!")

def define_experiment(exp_dict, args):
    set_dir_tools    (exp_dict, os.getcwd() + '/../../tools')
    set_dir_compile  (exp_dict, os.getcwd() + '/../../microbench')
    set_dir_run      (exp_dict, os.getcwd() + '/../../microbench')
    set_cmd_compile  (exp_dict, 'make -j ds-reclaim has_libpapi=0')
    set_dir_data     (exp_dict, os.getcwd() + '/data_unused')

    add_run_param    (exp_dict, 'TOTAL_THREADS', [min(120, int(sys.argv[1]))])
    add_run_param    (exp_dict, '__trials', [1])
    add_run_param    (exp_dict, 'DS_TYPENAME', ['brown_ext_chromatic_augment_lf', 'brown_ext_chromatic_delegateDouble_lf', 'brown_ext_chromatic_delegateSingle_lf', 'ellen_augmented_ext_bst_lf', 'wei_ext_vcas_bst_lf', 'arbel_int_bst_lf', 'blelloch_ext_btree_lf'])
    add_run_param    (exp_dict, 'MAXKEY', [100000]) 
    add_run_param    (exp_dict, 'RQSIZE', [50000])
    add_run_param    (exp_dict, 'ZIPF_PARAM', ['0'])
    add_run_param    (exp_dict, 'INS_DEL_FRAC', ['20.0 20.0'])
    add_data_field   (exp_dict, 'total_throughput', coltype='INTEGER')

    set_cmd_run      (exp_dict, 'LD_PRELOAD=../lib/libmimalloc.so ./bin/{DS_TYPENAME}.debra -nwork {TOTAL_THREADS} -nprefill 30 -insdel {INS_DEL_FRAC} -dist-zipf {ZIPF_PARAM} -autorq -rqsize {RQSIZE} -k {MAXKEY} -t 3000')
    
    add_plot_set(
            exp_dict
          , name='unused.pdf'
          , title='unused'
          , varying_cols_list=[]
          , series='DS_TYPENAME'
          , x_axis='TOTAL_THREADS'
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