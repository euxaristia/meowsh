#!/usr/bin/env bash
./meowsh <<EOF
echo HI_FROM_NON_INTERACTIVE
exit
EOF
echo "Non-interactive finished with status $?"

# Now try to simulate what script does
printf "echo HI_FROM_SIMULATED_INTERACTIVE
exit
" | ./meowsh
echo "Simulated interactive finished with status $?"
