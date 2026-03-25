#!/usr/bin/python3

import numpy as np
import sys
import getopt
import os
import fileinput
import argparse
import re
from io import StringIO

## TODO: bug fix -- single column of floats should not take before & after period as two different numbers.
## idea: try to automatically detect header by looking for the closest row to the first data-row
##       that contains the same fields (blobs & separators) as the first data-row!
## idea: consider allowing numbers containing commas... tradeoff is losing CSV support...

######################
## parse arguments
######################

parser = argparse.ArgumentParser(description='Produce a pandas bar plot from THREE COLUMN <series> <x> <y> or TWO COLUMN <x> <y> or TWO COLUMN <series> <y> or ONE COLUMN <y> data provided via a file or stdin (this attempts to be "smart" and figure out what your data is, attempting to identify which of these formats is most common in the input data, selecting that most populous format to filter based on, and ignoring lines of text that don\'t conform to that format. try passing unsanitized data to it and see...).')
parser.add_argument('-i', dest='infile', type=argparse.FileType('r'), default=sys.stdin, help='input file containing data; if none specified then will use stdin. (if your data columns are not in an order that produces desired results, try using awk to easily shuffle columns...)')
parser.add_argument('-o', dest='outfile', type=argparse.FileType('w'), default='_temp.png', help='output file with any image format extension such as .png or .svg; if none specified then _temp.png will be produced and imgcat will be attempted')
parser.add_argument('-t', dest='title', default="", help='title string for the plot')
parser.add_argument('--x-title', dest='x_title', default="", help='title for the x-axis')
parser.add_argument('--y-title', dest='y_title', default="", help='title for the y-axis')
parser.add_argument('--width', dest='width_inches', type=float, default=8, help='width in inches for the plot (at given dpi); default 8')
parser.add_argument('--height', dest='height_inches', type=float, default=6, help='height in inches for the plot (at given dpi); default 6')
parser.add_argument('--dpi', dest='dots_per_inch', type=int, default=100, help='DPI (dots per inch) to use for the plot; default 100')
parser.add_argument('--no-x-axis', dest='no_x_axis', action='store_true', help='disable the x-axis')
parser.set_defaults(no_x_axis=False)
parser.add_argument('--no-y-axis', dest='no_y_axis', action='store_true', help='disable the y-axis')
parser.set_defaults(no_y_axis=False)
parser.add_argument('--logy', dest='log_y', action='store_true', help='use a logarithmic y-axis')
parser.set_defaults(log_y=False)
parser.add_argument('--no-y-minor-ticks', dest='no_y_minor_ticks', action='store_true', help='force the logarithmic y-axis to include all minor ticks')
parser.set_defaults(no_y_minor_ticks=False)
parser.add_argument('--legend-only', dest='legend_only', action='store_true', help='use the data solely to produce a legend and render that legend')
parser.set_defaults(legend_only=False)
parser.add_argument('--legend-include', dest='legend_include', action='store_true', help='include a legend on the plot')
parser.set_defaults(legend_include=False)
parser.add_argument('--legend-columns', dest='legend_columns', type=int, default=1, help='number of columns to use to show legend entries')
parser.add_argument('--font-size', dest='font_size', type=int, default=20, help='font size to use in points (default: 20)')
parser.add_argument('--normalize-each-series', dest='normalize_each_series', action='store_true', help='normalize each series INDIVIDUALLY to maxval=1 (meaning they will all have the same max! but mins are not forced to 0, only scaled); really only useful for comparing how each series changes relative to ITSELF over the x-axis')
parser.set_defaults(normalize_each_series=False)
parser.add_argument('--lightmode', dest='lightmode', action='store_true', help='enable light mode (disable dark mode)')
parser.set_defaults(lightmode=False)
parser.add_argument('--no-y-grid', dest='no_y_grid', action='store_true', help='remove all grids on y-axis')
parser.set_defaults(no_y_grid=False)
parser.add_argument('--no-y-minor-grid', dest='no_y_minor_grid', action='store_true', help='remove grid on y-axis minor ticks')
parser.set_defaults(no_y_minor_grid=False)
parser.add_argument('--error-bar-width', dest='error_bar_width', type=float, default=6, help='set width of error bars (default 6); 0 will disable error bars')
parser.add_argument('--stacked', dest='stacked', action='store_true', help='causes bars to be stacked')
parser.set_defaults(stacked=False)
parser.add_argument('--ignore', dest='ignore', help='ignore the next argument')
parser.add_argument('--debug', dest='debug', action='store_true', help='enable debug printing')
parser.set_defaults(debug=False)
parser.add_argument('--style-hooks', dest='style_hooks', default='', help='allows a filename to be provided that implements functions style_init(mpl), style_before_plotting(mpl, plot_kwargs_dict) and style_after_plotting(mpl). your file will be imported, and hooks will be added so that your functions will be called to style the mpl instance. note that plt/fig/ax can all be extracted from mpl.')
args = parser.parse_args()

# parser.print_usage()
if len(sys.argv) < 2:
    if sys.stdin.isatty():
        parser.print_usage()
        print('waiting on stdin for data...')

######################
## setup matplotlib
######################

# print('args={}'.format(args))

import math
import matplotlib as mpl
mpl.use('Agg')
import matplotlib.pyplot as plt
import pandas as pd
import matplotlib.ticker as ticker
from matplotlib.ticker import FuncFormatter
from matplotlib import rcParams
mpl.rc('hatch', color='k', linewidth=1)

if args.style_hooks != '':
    sys.path.append(os.path.dirname(args.style_hooks))
    module_filename = os.path.relpath(args.style_hooks, os.path.dirname(args.style_hooks)).replace('.py', '')
    import importlib
    mod_style_hooks = importlib.import_module(module_filename)
    mod_style_hooks.style_init(mpl)

else:
    rcParams.update({'figure.autolayout': True})
    rcParams.update({'font.size': args.font_size})
    if not args.lightmode:
        plt.style.use('dark_background')
    plt.rcParams["figure.dpi"] = args.dots_per_inch



######################
## load data
######################

## note: this is way more complex than it needs to be...
## this attempts to be "smart" and figure out what your data is, allowing all sorts of weird formats...

# read all input
if args.infile.name == '<stdin>':
    lines = sys.stdin.readlines()
else:
    infile = open(args.infile.name, 'r')
    lines = infile.readlines()

# first try to filter lines as follows

# first determine whether the data is mostly three column, mostly 2 column (two numbers or series & number), or mostly 1 column (only numbers, one per row)

# re_number = r"[0-9]+\.[0-9]+e[0-9]+|[0-9]+e[0-9]+|[0-9]+\.[0-9]+|[0-9]+\.[0-9]*|[0-9]*\.[0-9]+|[0-9]+"
re_number = r"[0-9]+\.[0-9]+e[-]*[0-9]+|[0-9]+e[-]?[0-9]+|[0-9]+\.[0-9]+|[0-9]+"
prognumber = re.compile(re_number)
# 3col: find non whitespace non-numeric-initial string followed by [space, then] 2 numbers (with anything non-numeric and non-period (.) between them, and anything non-numeric after them)
prog3col = re.compile(r"^[\s]*([^\s0-9][^\s]*)[^0-9]*[\s]+({})[^\.0-9]+({})[^0-9]*$".format(re_number, re_number))
# 2colseries: find non whitespace non-numeric-initial string followed by [space, then] 1 number (with anything non-numeric between or after them)
prog2colS = re.compile(r"^[\s]*([^\s0-9][^\s]*)[^0-9]*[\s]+({})[^0-9]*$".format(re_number))
# 2colx: find 2 numbers (with no non-whitespace non-numeric string before the first, but with anything non-numeric and non-period (.) between them, and anything non-numeric after them)
prog2colX = re.compile(r"^[\s]*({})[^\.0-9]+({})[^0-9]*$".format(re_number, re_number))
# 1col: find 1 number only (with no non-numeric string before it)
prog1col = re.compile(r"^[\s]*({})[^0-9]*$".format(re_number))
# 3col numeric:
prog3numcol = re.compile(r"^[\s]*([^\s]+)[^0-9]*[\s]+({})[^\.0-9]+({})[^0-9]*$".format(re_number, re_number))
    ## the following is as originally designed and working for the majority of the time -- changed above to not catch lines of form "l3miss 54191" as 3 numbers...
    ## prog3numcol = re.compile(r"^[\s]*([^\s]+)[^0-9]*({})[^\.0-9]+({})[^0-9]*$".format(re_number, re_number))

countOfThreeCol = 0
countOfTwoColSeries = 0
countOfTwoColX = 0
countOfOneCol = 0
countOfThreeNumCol = 0
for line in lines:
    # line = line.replace(',', '') # consider allowing numbers containing commas... tradeoff is losing CSV support...
    line = line.strip()

    m = prog3col.match(line)
    if m and len(m.groups()) >= 3: countOfThreeCol = countOfThreeCol + 1

    m = prog2colS.match(line)
    if m and len(m.groups()) >= 2: countOfTwoColSeries = countOfTwoColSeries + 1

    m = prog2colX.match(line)
    if m and len(m.groups()) >= 2: countOfTwoColX = countOfTwoColX + 1

    m = prog1col.match(line)
    if m and len(m.groups()) >= 1: countOfOneCol = countOfOneCol + 1

    m = prog3numcol.match(line)
    if m and len(m.groups()) >= 3: countOfThreeNumCol = countOfThreeNumCol + 1


# print(lines)
if args.debug:
    print('3col={} 2colS={} 2colX={} 1col={} 3colNum={}'.format(countOfThreeCol, countOfTwoColSeries, countOfTwoColX, countOfOneCol, countOfThreeNumCol))

keepLines = []
maxCount = max(countOfThreeCol, countOfTwoColSeries, countOfTwoColX, countOfOneCol, countOfThreeNumCol)
series_row_counts = dict() ## in case we have <series> <y> data... need to avoid creating separate unique x values across all <series> <y> pairs...
for line in lines:
    # line = line.replace(',', '') # consider allowing numbers containing commas... tradeoff is losing CSV support...
    line = line.strip()

    if maxCount == countOfThreeCol:
        m = prog3col.match(line)
        if m and len(m.groups()) >= 3:
            ## experimental "series" detection: everything not matched by re_number regex
            s = ''.join(prognumber.split(line)).replace(' ', '_')
            # s = m.group(1)

            keepLines.append('{} {} {}'.format(s, m.group(2), m.group(3)))
            # print('s="{}" line="{}" groups="{}"'.format(s, line.strip(), m.groups()))
            # print(m.group(0))
    elif maxCount == countOfThreeNumCol:
        m = prog3numcol.match(line)
        if m and len(m.groups()) >= 3:
            # ## experimental "series" detection: everything not matched by re_number regex
            # s = ''.join(prognumber.split(line)).replace(' ', '_')
            s = m.group(1)

            keepLines.append('{} {} {}'.format(s, m.group(2), m.group(3)))
            # print('s="{}" line="{}" groups="{}"'.format(s, line.strip(), m.groups()))
            # print(m.group(0))
    elif maxCount == countOfTwoColSeries:
        m = prog2colS.match(line)
        if m and len(m.groups()) >= 2:
            ## experimental "series" detection: everything not matched by re_number regex
            s = ''.join(prognumber.split(line)).replace(' ', '_')
            # s = m.group(1)

            if s not in series_row_counts: series_row_counts[s] = 0
            series_row_counts[s] += 1
            keepLines.append('{} {} {}'.format(m.group(1), series_row_counts[s], m.group(2)))
            # print('s="{}" line="{}" groups="{}"'.format(s, line.strip(), m.groups()))
    elif maxCount == countOfTwoColX:
        m = prog2colX.match(line)
        if m and len(m.groups()) >= 2:
            keepLines.append('{} {} {}'.format('data', m.group(1), m.group(2)))
            # print('"{}" {}'.format(line.strip(), m.groups()))
            args.legend_include = False ##### OVERRIDE BECAUSE THERE IS NO MEANINGFUL SERIES!!
    elif maxCount == countOfOneCol:
        m = prog1col.match(line)
        if m and len(m.groups()) >= 1:
            keepLines.append('{} {} {}'.format('data', len(keepLines), m.group(1)))
            # print('"{}" {}'.format(line.strip(), m.groups()))
            args.legend_include = False ##### OVERRIDE BECAUSE THERE IS NO MEANINGFUL SERIES!!

csvStringIO = StringIO('\n'.join(keepLines))
# csvStringIO = StringIO(''.join(lines))

data = pd.read_csv(csvStringIO, names=['series', 'x', 'y'], sep=' ', index_col=None)
# print(data.head())

## OLDER SOMEWHAT SIMPLIFIED DATA INTAKE

# data = pd.read_csv(args.infile, names=['series', 'x', 'y'], sep=' ', index_col=None)

# # check for null/nan in any cells (to see if it's NOT well formed 3-col data)
# if data.isnull().values.any():
#     ## could be well-formed two column or one column data. try parsing as two cols and check.

#     data_old = data
#     data['y'] = data['x'] #.drop(['y'], axis=1)
#     if data.isnull().values.any():
#         ## null/nan found under two column hypothesis. must be 1-column data or invalid.
#         # print("2-col NaNs found")

#         # proceed under 1-column hypothesis... only sensible interpretation is y-values...
#         # so manufacture dummy series and 1..range(len(input)) x values

#         data['y'] = data['series'] ## column 1 was loaded into series, but it's y-values...
#         # series = []
#         # print(data)
#         # series_row_counts = dict()
#         # for index, row in data.iterrows():
#         #     s = row['series'] ## actually y-value
#         #     # print('row: {}'.format(row))
#         data['x'] = range(len(data))
#         # data['y'] = data['series']
#         data['series'] = [ "data" for unused in data['y'] ]


#         # print(data)

#         if data.isnull().values.any():
#             # still garbage found under single column hypothesis
#             print("ERROR: you must provide valid one, two or three column data. Invalid parsed data:")
#             print(data_old)
#             exit(1)

#     else:
#         ## data two columns. assuming it's series and y values, add an x column.
#         x = []
#         # print(data)
#         series_row_counts = dict()
#         for index, row in data.iterrows():
#             s = row['series']
#             if s not in series_row_counts: series_row_counts[s] = 0
#             series_row_counts[s] += 1
#             x.append(series_row_counts[s])
#             # print('row: {}'.format(row))
#         data['x'] = x
#         # print(data)

#         ## TODO: deal with 2-column format with x and y values but not series

## should have three column data at this point

# print(data)

# def is_number(s):
#     try:
#         float(s)
#         return True
#     except ValueError:
#         return False

# for index, row in data.iterrows():
#     if not is_number(repr(row['y'])):
#         m = re.search(r"[0-9]+|[0-9]+\.[0-9]*|[0-9]*\.[0-9]+|[0-9]+e[0-9]+", row['y'])
#         if m:
#             foundnum = m.group(0)
#             if foundnum and len(foundnum) > 0:
#                 data.at[index, 'y'] = float(foundnum)
#         else:
#             print("ERROR: could not find number in row '{}' for the following data.".format(row))
#             exit(1)

# convert column data types to numeric (in case its needed)
data['x'] = pd.to_numeric(data['x'])
data['y'] = pd.to_numeric(data['y'])

# print('data after smart number finding')
# print(data)

## compute mean data points (grouping/pivoting as appropriate)
tmean = pd.pivot_table(data, columns='series', index='x', values='y', aggfunc='mean')

if args.normalize_each_series:
    tnormalized=tmean/tmean.max() ## normalize all data into range (-infty,1] so maxval = 1 (after minval is divided by maxval)
    # tnormalized=(tmean-tmean.min())/(tmean.max()-tmean.min()) ## normalize all data into range 0-1 so minval = 0 and maxval = 1 for each series
    # tminx = tmin.min()
    # tmaxx = tmax.max()
    # print('minx={}'.format(tminx))
    # print('maxx={}'.format(tmaxx))
    tmean = tnormalized
    # print('tmean={}'.format(tmean))

# tmin = tmean.min()
# tmax = tmean.max()

tmin = pd.pivot_table(data, columns='series', index='x', values='y', aggfunc='min')
tmax = pd.pivot_table(data, columns='series', index='x', values='y', aggfunc='max')

# print(tmean.head())
# print(tmin.head())
# print(tmax.head())

# ## sort dataframes by algorithms in this order:
# tmean = tmean.reindex(algs, axis=1)
# tmin = tmin.reindex(algs, axis=1)
# tmax = tmax.reindex(algs, axis=1)
# print(tmean.head())

## compute error bars
tpos_err = tmax - tmean
tneg_err = tmean - tmin
err = [[tneg_err[c], tpos_err[c]] for c in tmean]
# print("error bars {}".format(err))

if not len(data):
    print("ERROR: no data provided, so no graph to render.")
    quit()

if args.error_bar_width > 0 and len(err):
    for e in err[0]:
        if len([x for x in e.index]) <= 1:
            print("note : forcing NO error bars because index is too small: {}".format(e.index))
            args.error_bar_width = 0
elif not len(err):
    args.error_bar_width = 0

######################
## setup plot
######################

plot_kwargs = dict(
      legend=False
    , title=args.title
    , kind='bar'
    , figsize=(args.width_inches, args.height_inches)
    , width=0.75
    , edgecolor='black'
    , linewidth=1.5
    , zorder=10
    , logy=args.log_y
)
if args.stacked: plot_kwargs['stacked'] = True

legend_kwargs = dict(
      title=None
    , loc='upper center'
    , bbox_to_anchor=(0.5, -0.1)
    , fancybox=True
    , shadow=True
    , ncol=args.legend_columns
)

fig, ax = plt.subplots()
if args.style_hooks != '': mod_style_hooks.style_before_plotting(mpl, plot_kwargs, legend_kwargs)

if args.error_bar_width == 0:
    chart = tmean.plot(fig=fig, ax=ax, **plot_kwargs)
else:
    plot_kwargs['yerr'] = err
    plot_kwargs['error_kw'] = dict(elinewidth=args.error_bar_width, ecolor='red')
    # orig_cols = [col for col in tmean.columns]
    # tmean.columns = ["_" + col for col in tmean.columns]
    # chart = tmean.plot(ax=ax, legend=False, title=args.title, kind='bar', yerr=err, error_kw=dict(elinewidth=args.error_bar_width+4, ecolor='black',capthick=2,capsize=(args.error_bar_width+2)/2), figsize=(args.width_inches, args.height_inches), width=0.75, edgecolor='black', linewidth=3, zorder=10, logy=args.log_y)
    # ## replot error bars for a stylistic effect, but MUST prefix columns with "_" to prevent duplicate legend entries
    # tmean.columns = orig_cols
    chart = tmean.plot(fig=fig, ax=ax, **plot_kwargs)

chart.grid(axis='y', zorder=0)

ax = plt.gca()
if args.normalize_each_series:
    ax.set_ylim(top=1)

# ax.yaxis.get_offset_text().set_y(-100)
# ax.yaxis.set_offset_position("left")

# ax.yaxis.get_offset_text().set_y(-100)
# ax.yaxis.set_offset_position("left")

# i=0
# for c in tmean:
#     # print("c in tmean={}".format(c))
#     # print("tmean[c]=")
#     df=tmean[c]
#     # print(df)
#     # print("trying to extract column:")
#     # print("tmean[{}]={}".format(c, tmean[c]))
#     xvals=[x for x in df.index]
#     yvals=[y for y in df]
#     errvals=[[e for e in err[i][0]], [e for e in err[i][1]]]
#     print("xvals={} yvals={} errvals={}".format(xvals, yvals, errvals))
#     ax.errorbar(xvals, yvals, yerr=errvals, linewidth=args.error_bar_width, color='red', zorder=20)
#     i=i+1

chart.set_xticklabels(chart.get_xticklabels(), ha="center", rotation=0)

bars = ax.patches
patterns =( 'x', '/', '//', 'O', 'o', '\\', '\\\\', '-', '+', ' ' )
hatches = [p for p in patterns for i in range(len(tmean))]
for bar, hatch in zip(bars, hatches):
    bar.set_hatch(hatch)

## maybe remove y grid

# print("args.no_y_grid={} args.no_y_minor_grid={}".format(args.no_y_grid, args.no_y_minor_grid))
if not args.no_y_grid:
    plt.grid(axis='y', which='major', linestyle='-')
    if not args.no_y_minor_grid:
        plt.grid(axis='y', which='minor', linestyle='--')

## maybe add all-ticks for logy

if not args.no_y_minor_ticks:
    if args.log_y:
        ax.yaxis.set_minor_locator(ticker.LogLocator(subs="all"))
    else:
        ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())

## maybe remove axis tick labels

if args.no_x_axis:
    plt.setp(ax.get_xticklabels(), visible=False)
if args.no_y_axis:
    plt.setp(ax.get_yticklabels(), visible=False)

## set x axis title

if args.x_title == "" or args.x_title == None:
    ax.xaxis.label.set_visible(False)
else:
    ax.xaxis.label.set_visible(True)
    ax.set_xlabel(args.x_title)

## set y axis title

if args.y_title == "" or args.y_title == None:
    ax.yaxis.label.set_visible(False)
else:
    ax.yaxis.label.set_visible(True)
    ax.set_ylabel(args.y_title)

## save plot

if args.legend_include:
    plt.legend(**legend_kwargs)

if not args.legend_only:
    if args.style_hooks != '': mod_style_hooks.style_after_plotting(mpl)

    if args.outfile.name != "_temp.png":
        print("saving figure {}".format(args.outfile.name))
    plt.savefig(args.outfile.name)

######################
## handle legend-only
######################

if args.legend_only:
    handles, labels = ax.get_legend_handles_labels()
    fig_legend = plt.figure() #figsize=(12,1.2))
    axi = fig_legend.add_subplot(111)
    fig_legend.legend(handles, labels, loc='center', ncol=legend_kwargs['ncol'], frameon=False)
    # fig_legend.legend(handles, labels, loc='center', ncol=int(math.ceil(len(tpos_err)/2.)), frameon=False)
    axi.xaxis.set_visible(False)
    axi.yaxis.set_visible(False)
    axi.axes.set_visible(False)
    fig_legend.savefig(args.outfile.name, bbox_inches='tight')

# print('debug: args.outfile.name={}'.format(args.outfile.name))
if args.outfile.name == "_temp.png":
    os.system("imgcat _temp.png")
