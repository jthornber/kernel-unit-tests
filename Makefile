dm-multisnap-metadata-test-y  += multisnap-metadata-test.o dm-multisnap-metadata.o

obj-m += block-manager-test.o
obj-m += transaction-manager-test.o
obj-m += btree-test.o
obj-m += space-map-test.o
obj-m += dm-multisnap-metadata-test.o
#obj-m += thinp-metadata-test.o
