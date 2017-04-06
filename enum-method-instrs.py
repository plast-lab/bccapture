#!/usr/bin/python

# Utility that reads a .class file, disassembles it with javap,
# extracts the body of method m, and prints all occurences of an
# instruction type. Every occurence is uniquely numbered and is paired
# with its bytecode position.  This script can be used to connect
# invoke* instructions from bytecode to Jimple for Doop.
#
# Example. Looking for the return variable of invokevirtual of method
# Main.main() at bytecode position 5, we run:
#
# ./enum-method-instrs.py Main.class 'main(java.lang.String[])' invokevirtual
# ('bytecode index: 1', 'instr: invokevirtual/0')
# ('bytecode index: 5', 'instr: invokevirtual/1')
#

import sys
import subprocess

if len(sys.argv) != 4:
    print("Usage: ./find-method-num.sh path/to/C.class method instruction")
    sys.exit(-1)

classFile = sys.argv[1]
methodToSearch = sys.argv[2]
instrType = sys.argv[3]

result = subprocess.check_output(['javap', '-p', '-c', classFile])
# print(result)

methodToSearchTxt1 = ' ' + methodToSearch + ';'
methodToSearchTxt2 = ' ' + methodToSearch + ' throws'
lines = result.splitlines()
i = 0
counter = 0
while (i < len(lines)):
    # print(i)
    if (lines[i].endswith(methodToSearchTxt1)) or (lines[i].find(methodToSearchTxt2) != -1):
        print('Occurences of ' + instrType + ' in method ' + methodToSearch)
        i += 1
        if (lines[i].endswith('Code:')):
            i += 1
            while (i < len(lines)):
                if (lines[i].find(': ') != -1):
                    fields1 = lines[i].split(':')
                    instr = fields1[1]
                    if (instr.find(instrType) != -1):
                        bytecodeIdx = int(fields1[0])
                        print('bytecode index: ' + str(bytecodeIdx), 'instr: ' + instrType + '/' + str(counter))
                        counter += 1
                    i += 1
                else:
                    sys.exit(0)
        else:
            print("Error reading disassembled output.")
            sys.exit(-2)
    i += 1

# print Instr
# javap -p -c ${CLASS} | grep 

