for proto in *.proto; do
    protoc --plugin=protoc-gen-nanopb=$(which protoc-gen-nanopb) \
           --nanopb_out=. \
           -I. \
           $proto
done