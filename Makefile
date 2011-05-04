dm-transaction-manager-test-y := transaction-manager-test.o dm-space-map-core.o
dm-btree-test-y := btree-test.o dm-space-map-core.o
dm-space-map-test-y := space-map-test.o dm-space-map-core.o
dm-multisnap-metadata-test-y := multisnap-metadata-test.o dm-multisnap-metadata.o

obj-m += block-manager-test.o
obj-m += dm-transaction-manager-test.o
obj-m += dm-btree-test.o
obj-m += dm-space-map-test.o
obj-m += dm-multisnap-metadata-test.o
#obj-m += thinp-metadata-test.o
