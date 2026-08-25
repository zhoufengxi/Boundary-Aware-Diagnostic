make clean
rm ./reports -rf
rm ./html_out -rf
rm ./compile_commands.json -f
bear  make 



CodeChecker analyze \
  --ctu \
  --ctu-ast-mode load-from-pch \
  compile_commands.json \
  -o reports \
  --disable-all \
  --enable cplusplus.NewDeleteLeaks \
  --enable cplusplus.NewDelete \
  --keep-gcc-intrin \
  --jobs 32 \
  --verbose debug
CodeChecker parse -e html -o html_out reports


python ./collect_all_reports.py 
