#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef uint16_t u16;
struct idpf_queue;
struct idpf_q_vector {
	struct idpf_queue *rx[4], *tx[4], *bufq[4];
	u16 num_rxq, num_txq, num_bufq;
};
struct idpf_queue { struct idpf_q_vector *q_vector; };
struct idpf_rxq_set { struct idpf_queue rxq; };
struct idpf_bufq_set { struct idpf_queue bufq; };
struct idpf_rxq_group {
	struct { u16 num_rxq; struct idpf_queue **rxqs; } singleq;
	struct {
		u16 num_rxq_sets;
		struct idpf_rxq_set **rxq_sets;
		struct idpf_bufq_set *bufq_sets;
	} splitq;
};
struct idpf_txq_group {
	u16 num_txq;
	struct idpf_queue **txqs;
	struct idpf_queue *complq;
};
struct idpf_q_vec_rsrc {
	bool rxq_model, txq_model;
	u16 num_rxq_grp, num_txq_grp, num_bufqs_per_qgrp, num_q_vectors;
	struct idpf_rxq_group *rxq_grps;
	struct idpf_txq_group *txq_grps;
	struct idpf_q_vector *q_vectors;
};
#define idpf_is_queue_model_split(model) (model)
#include "vectors_under_test.inc"

int
main(void)
{
	struct idpf_queue rxq = { 0 }, txq = { 0 };
	struct idpf_queue *rxqs[] = { &rxq }, *txqs[] = { &txq };
	struct idpf_rxq_set rxset = { 0 }, *rxsets[] = { &rxset };
	struct idpf_bufq_set bufset = { 0 };
	struct idpf_rxq_group rxgroup = {
		.singleq = { 1, rxqs }, .splitq = { 1, rxsets, &bufset }
	};
	struct idpf_txq_group txgroup = { 1, txqs, &txq };
	struct idpf_q_vector vector = { 0 };
	struct idpf_q_vec_rsrc rsrc = {
		.num_rxq_grp = 1, .num_txq_grp = 1, .num_bufqs_per_qgrp = 1,
		.num_q_vectors = 1, .rxq_grps = &rxgroup, .txq_grps = &txgroup,
		.q_vectors = &vector
	};
	int failures = 0;

	for (int model = 0; model < 2; model++) {
		rsrc.rxq_model = rsrc.txq_model = model;
		vector = (struct idpf_q_vector){ 0 };
		for (int attempt = 0; attempt < 3; attempt++) {
			idpf_vport_intr_map_vector_to_qs(&rsrc);
			if (vector.num_rxq != 1 || vector.num_txq != 1 ||
			    vector.num_bufq != model || vector.tx[0] != &txq ||
			    vector.rx[0] != (model ? &rxset.rxq : &rxq)) {
				fprintf(stderr, "FAIL model=%d attempt=%d counts=%u/%u/%u\n",
				    model, attempt, vector.num_rxq, vector.num_txq,
				    vector.num_bufq);
				failures++;
			}
		}
	}
	printf("Vector reinitialization: %d failures\n", failures);
	return (failures != 0);
}