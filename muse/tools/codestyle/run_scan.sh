UNCRUSTIFY_BIN=$1
CONFIG=$2
UNTIDY_FILE=$3
DIR=$4

echo "UNCRUSTIFY_BIN: $UNCRUSTIFY_BIN"
echo "CONFIG: $CONFIG"
echo "UNTIDY_FILE: $UNTIDY_FILE"
echo "DIR: $DIR"

scan() {
    find -L "$DIR" -type d -exec test -e "{}/$UNTIDY_FILE" ';' -prune -o \
         -type f '(' "$@" ')' -print
}

scan -name '*.cpp' -o -name '*.c' -o -name '*.cc' -o -name '*.hpp' -o -name '*.h' \
  | xargs -n 1 -P 16 $UNCRUSTIFY_BIN -c $CONFIG --no-backup -l CPP

code=$?
if [[ ${code} -ne 0 ]]; then
    exit $code
fi

scan -name '*.mm' \
  | xargs -n 1 -P 16 $UNCRUSTIFY_BIN -c $CONFIG --no-backup -l OC+

code=$?
if [[ ${code} -ne 0 ]]; then
    exit $code
fi
