#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include "onic_json.h"
#include "jsmn.h"

static bool jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
	if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start
	    && strncmp(json + tok->start, s, tok->end - tok->start) == 0)
		return true;
	else
		return false;
}

#define PARSEBUF_LEN 32
int onic_get_platform_info(char *fname, struct onic_platform_info *pinfo)
{
	int err = 0;
	char fw_name[256];
	const struct firmware *fw;
	const char *jsonBuffer;
	u16 length;
	jsmn_parser parser;
	jsmntok_t *tokens;
	u16 numTokens;
	char parsingBuffer[PARSEBUF_LEN];
	int i,j;

	snprintf(fw_name, sizeof(fw_name), "xilinx/%s", fname);

	err = request_firmware(&fw, fw_name, NULL);
	if (err) {
		pr_err("%s: request_firmware failed : %d\n", __func__, err);
		return err;
	}
	jsonBuffer = fw->data;
	length = fw->size;

	/* initialize the JSMN parser and determine the number of JSON tokens */
	jsmn_init(&parser);
	numTokens = jsmn_parse(&parser, jsonBuffer, length, NULL, 0);

	/* The JSON file must be tokenized successfully. */
	if (numTokens < 1) {
		pr_err("%s: 1st jsmn_parse failed : %d\n", __func__, numTokens);
		err = -EINVAL;
		goto func_exit;
	}

	/* allocate space for tokens */
	tokens = (jsmntok_t *)kcalloc(numTokens, sizeof(jsmntok_t), GFP_KERNEL);
	if (tokens == NULL) {
		pr_err("%s: tokens array allocation failed.\n", __func__);
		err = -ENOMEM;
		goto func_exit;
	}

	/* initialize the JSMN paser and parse the json file into the tokens
	 * array */
	jsmn_init(&parser);
	numTokens = jsmn_parse(&parser, jsonBuffer, length, tokens, numTokens);

	/* The top-level element must be an object. */
	if (numTokens < 1 || tokens[0].type != JSMN_OBJECT) {
		kfree(tokens);
		pr_err("%s: 2nd jsmn_parse failed : %d\n", __func__, numTokens);
		err = -EINVAL;
		goto func_exit;
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < numTokens; i++) {
		if (jsoneq(jsonBuffer, &tokens[i], "qdma_bar")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtou8(parsingBuffer, 10, &pinfo->qdma_bar);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "user_bar")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtou8(parsingBuffer, 10, &pinfo->user_bar);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "queue_base")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtou16(parsingBuffer, 10, &pinfo->queue_base);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "queue_max")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtou16(parsingBuffer, 10, &pinfo->queue_max);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "used_queues")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtou16(parsingBuffer, 10, &pinfo->used_queues);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "pci_msix_user_cnt")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtou8(parsingBuffer, 10, &pinfo->pci_msix_user_cnt);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "pci_master_pf")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			pinfo->pci_master_pf = (parsingBuffer[0] != '0') &&
				(parsingBuffer[0] != 'f') && (parsingBuffer[0]
							      != 'F');
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "poll_mode")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			pinfo->poll_mode = (parsingBuffer[0] != '0') &&
				(parsingBuffer[0] != 'f') && (parsingBuffer[0]
							      != 'F');
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "intr_mod_en")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			pinfo->intr_mod_en = (parsingBuffer[0] != '0') &&
				(parsingBuffer[0] != 'f') && (parsingBuffer[0]
							      != 'F');
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "ring_sz")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtoint(parsingBuffer, 10, &pinfo->ring_sz);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "c2h_tmr_cnt")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtoint(parsingBuffer, 10, &pinfo->c2h_tmr_cnt);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "c2h_cnt_thr")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				tokens[i+1].end - tokens[i+1].start, jsonBuffer
				+ tokens[i+1].start);
			kstrtoint(parsingBuffer, 10, &pinfo->c2h_cnt_thr);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "c2h_buf_sz")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtoint(parsingBuffer, 10, &pinfo->c2h_buf_sz);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "rsfec_en")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			pinfo->rsfec_en = (parsingBuffer[0] != '0') &&
				(parsingBuffer[0] != 'f') && (parsingBuffer[0]
							      != 'F');
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "port_id")) {
			snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
				 tokens[i+1].end - tokens[i+1].start, jsonBuffer
				 + tokens[i+1].start);
			kstrtou8(parsingBuffer, 10, &pinfo->port_id);
			i++;
		} else if (jsoneq(jsonBuffer, &tokens[i], "mac_addr")) {
			if (tokens[i+1].type != JSMN_ARRAY) {
				continue; /* We expect groups to be an array of
					     strings */
			}
			for (j = 0; j < tokens[i+1].size; j++) {
				snprintf(parsingBuffer, PARSEBUF_LEN, "%.*s",
					 tokens[i+j+2].end -
					 tokens[i+j+2].start, jsonBuffer +
					 tokens[i+j+2].start);
				kstrtou8(parsingBuffer, 16,
					 &pinfo->mac_addr[j]);
			}
			i += tokens[i+1].size + 1;
		} else {
			pr_err("%s: Unexpected key: %.*s\n", __func__,
			       tokens[i].end - tokens[i].start,
			       jsonBuffer + tokens[i].start);
		}
	}

func_exit:
	release_firmware(fw);
	return err;
}
