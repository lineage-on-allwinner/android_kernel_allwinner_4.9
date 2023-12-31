/*
 * Copyright (C) 2019 Allwinnertech, <liuli@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include "include/eink_driver.h"
#include "include/fmt_convert.h"
#include "include/eink_sys_source.h"
#include "lowlevel/eink_reg.h"

static struct eink_manager *g_eink_manager;


#ifdef VIRTUAL_REGISTER
static void *test_preg_base;
static void *test_vreg_base;
#endif

struct eink_manager *get_eink_manager(void)
{
	return g_eink_manager;
}

static int detect_fresh_thread(void *parg)
{
	int ret = 0;
#ifdef OFFLINE_SINGLE_MODE
	unsigned long flags = 0;
#endif
	u32 temperature = 28;
	int pipe_id = -1;
	u32 request_fail_cnt = 0;
	struct eink_manager *eink_mgr = NULL;
	struct buf_manager *buf_mgr = NULL;
	struct pipe_manager *pipe_mgr = NULL;
	struct index_manager *index_mgr = NULL;
	struct timing_ctrl_manager *timing_ctrl_mgr = NULL;
	struct img_node *curnode = NULL;
	struct eink_img *cur_img = NULL;
	struct pipe_info_node pipe_info;

	u64 free_pipe_state = 0;
	u32 buf_size = 0;

	static bool first_fresh = true;
	bool can_upd_flag = 0;

#ifdef REGISTER_PRINT
	u32 reg_base = 0, reg_end;
#endif
	eink_mgr = (struct eink_manager *)parg;
	if (eink_mgr == NULL) {
		pr_err("%s: eink manager is not initial\n", __func__);
		return -1;
	}

	buf_mgr = eink_mgr->buf_mgr;
	pipe_mgr = eink_mgr->pipe_mgr;
	index_mgr = eink_mgr->index_mgr;
	timing_ctrl_mgr = eink_mgr->timing_ctrl_mgr;

	for (;;) {
		if (buf_mgr->is_upd_list_empty(buf_mgr) && buf_mgr->is_coll_list_empty(buf_mgr)) {
			if (!first_fresh) {
				EINK_DEFAULT_MSG("upd list and coll list both empty!\n");
			}
			msleep(1);
			continue;
		}

		free_pipe_state = pipe_mgr->get_free_pipe_state(pipe_mgr);
		can_upd_flag = buf_mgr->check_upd_coll_state(buf_mgr, free_pipe_state);
		EINK_DEFAULT_MSG("free_pipe_state = 0x%llx, coll can upd_flag = %d\n",
				free_pipe_state, can_upd_flag);

		if ((can_upd_flag == false) && buf_mgr->is_upd_list_empty(buf_mgr)) {
			msleep(1);
			continue;
		}

		curnode = buf_mgr->get_img_from_coll_list(buf_mgr);
		if (curnode == NULL) {
			EINK_INFO_MSG("COLL MAY HAVE BUT NOT!\n");
			curnode = buf_mgr->get_img_from_upd_list(buf_mgr);
			if (curnode == NULL) {
				msleep(1);
				continue;
			} else {
				EINK_INFO_MSG("img node from upd list\n");
				curnode->update_master = 0;
			}
		} else {
			EINK_INFO_MSG("img node from coll list\n");
			curnode->update_master = 1;
		}

		if (first_fresh) {
			index_mgr->set_rmi_addr(index_mgr);
		}

		temperature = eink_mgr->get_temperature(eink_mgr);

		cur_img = curnode->img;

		EINK_INFO_MSG("Cur_img addr = 0x%p\n", cur_img->paddr);
		if (curnode->update_master == 1) {
			eink_put_gray_to_mem(curnode->upd_order, (char *)cur_img->vaddr,
					buf_mgr->width, buf_mgr->height);
		}

		memset(&pipe_info, 0, sizeof(struct pipe_info_node));
		pipe_info.img = cur_img;
		memcpy(&pipe_info.upd_win, &cur_img->upd_win, sizeof(struct upd_win));
		pipe_info.upd_mode = cur_img->upd_mode;

		eink_get_wf_data(pipe_info.upd_mode, temperature,
				&pipe_info.total_frames, &pipe_info.wav_paddr,
				&pipe_info.wav_vaddr);/* 还没想好结果 fix me */

		EINK_INFO_MSG("temp=%d, mode=0x%x, total=%d, waveform_paddr=0x%x, waveform_vaddr=0x%x\n",
				temperature, pipe_info.upd_mode,
				pipe_info.total_frames, (unsigned int)pipe_info.wav_paddr,
				(unsigned int)pipe_info.wav_vaddr);
#ifdef WAVEDATA_DEBUG
		save_waveform_to_mem(curnode->upd_order,
					(u8 *)pipe_info.wav_vaddr,
					pipe_info.total_frames,
					eink_mgr->panel_info.bit_num);
		if (pipe_info.upd_mode == EINK_INIT_MODE) {
			EINK_INFO_MSG("get_gray_mem mode = %d\n", pipe_info.upd_mode);
			eink_get_gray_from_mem((u8 *)pipe_info.wav_vaddr,
					DEFAULT_INIT_WAV_PATH,
					(pipe_info.total_frames * 256 / 4), 0);
		} else if (pipe_info.upd_mode == EINK_GC16_MODE) {
			EINK_INFO_MSG("get_gray_mem mode = %d\n", pipe_info.upd_mode);
			eink_get_gray_from_mem((u8 *)pipe_info.wav_vaddr,
					DEFAULT_GC16_WAV_PATH,
					(51 * 64), 0);
		}
#endif
		pipe_id = -1;
		while (pipe_id < 0 && request_fail_cnt < REQUST_PIPE_FAIL_MAX_CNT) {
			pipe_id = pipe_mgr->request_pipe(pipe_mgr);
			if (pipe_id < 0) {
				request_fail_cnt++;
				msleep(5);
				continue;
			}
		}
		if (pipe_id < 0) {
			pr_err("Request free pipe failed!\n");
			ret = -1;
			goto err_out;
		}

		pipe_info.pipe_id = pipe_id;

		/* config pipe */
		ret = pipe_mgr->config_pipe(pipe_mgr, pipe_info);

		/* enable pipe */
		ret = pipe_mgr->active_pipe(pipe_mgr, pipe_info.pipe_id);

#ifdef OFFLINE_MULTI_MODE
		if (first_fresh) {
			ret = request_multi_frame_buffer(pipe_mgr, &timing_ctrl_mgr->info);
			if (ret) {
				pr_err("request buf for offline molti mode fail!\n");
				goto err_out;
			}
			EINK_INFO_MSG("request buffer paddr=%p,vaddr=%p\n", pipe_mgr->dec_wav_paddr, pipe_mgr->dec_wav_vaddr);
			eink_edma_cfg(&eink_mgr->panel_info);
			eink_edma_start();
			eink_prepare_decode((unsigned long)pipe_mgr->dec_wav_paddr, &pipe_mgr->panel_info);
		}
#elif defined OFFLINE_SINGLE_MODE
		spin_lock_irqsave(&pipe_mgr->list_lock, flags);
		EINK_INFO_MSG("OFFLINE_SINGLE_MODE!\n");
		if (pipe_mgr->ee_processing == false) {
			pipe_mgr->dec_wav_paddr =
				request_buffer_for_decode(&pipe_mgr->wavedata_ring_buffer, &pipe_mgr->dec_wav_vaddr);
			if (pipe_mgr->dec_wav_paddr == NULL) {
				pr_err("[%s]:request buf for offline single mode dec fail!\n", __func__);
				ret = -1;
				goto err_out;
			}

			EINK_INFO_MSG("request buffer paddr=0x%p,vaddr=0x%p\n",
						pipe_mgr->dec_wav_paddr,
						pipe_mgr->dec_wav_vaddr);
			eink_prepare_decode((unsigned long)pipe_mgr->dec_wav_paddr, &pipe_mgr->panel_info);
			if (!first_fresh) {
				eink_start();
			}

			pipe_mgr->ee_processing = true;
		}
		spin_unlock_irqrestore(&pipe_mgr->list_lock, flags);
#endif

		if (first_fresh) {
			/* enable eink engine */
			EINK_INFO_MSG("Eink start!\n");
			eink_start();
		}

#ifdef REGISTER_PRINT
		reg_base = eink_get_reg_base();
		EINK_INFO_MSG("reg_base = 0x%x\n", reg_base);
		reg_end = reg_base + 0x3ff;
		eink_print_register(reg_base, reg_end);
#endif
#if 0
		if (!first_fresh) {
			wait_event_interruptible(eink_mgr->upd_pic_queue,
					((eink_mgr->upd_coll_win_flag == 1) ||
					 (eink_mgr->upd_pic_fin_flag == 1)));
			if (eink_mgr->upd_coll_win_flag == 1)
				eink_mgr->upd_coll_win_flag = 0;
			else if (eink_mgr->upd_pic_fin_flag == 1)
				eink_mgr->upd_pic_fin_flag = 0;
#ifdef INDEX_DEBUG
			save_upd_rmi_buffer(curnode->upd_order,
						index_mgr->rmi_vaddr,
						buf_mgr->width * buf_mgr->height * 2);
#endif
		}
#endif
		/* 释放buf */
		if (buf_mgr->processing_img_node && buf_mgr->processing_img_node->img) {
			buf_size = buf_mgr->width * buf_mgr->height;
			EINK_INFO_MSG("prcessing node wincalc_fin = %d, mode_select_fin = %d, coll_flag = 0x%llx\n",
					buf_mgr->processing_img_node->img->win_calc_fin,
					buf_mgr->processing_img_node->img->mode_select_fin,
					buf_mgr->processing_img_node->coll_flag);
			switch (buf_mgr->processing_img_node->update_master) {
			case 0:
				if ((buf_mgr->processing_img_node->img->win_calc_fin == true) &&
						(buf_mgr->processing_img_node->img->mode_select_fin == true) &&
						(buf_mgr->processing_img_node->coll_flag == 0)) {/* fix me */
					if (buf_mgr->processing_img_node->extra_flag == true) {
						buf_mgr->processing_img_node->extra_flag = false;
						__list_del_entry(&buf_mgr->processing_img_node->node);
						eink_free(buf_mgr->processing_img_node->img->vaddr,
								buf_mgr->processing_img_node->img->paddr, buf_size);
						kfree(buf_mgr->processing_img_node->img->eink_hist);
						kfree(buf_mgr->processing_img_node->img);
						kfree(buf_mgr->processing_img_node);
					} else if (buf_mgr->processing_img_node->img->de_bypass_flag == true) {
						buf_mgr->processing_img_node->img->de_bypass_flag = false;
						__list_del_entry(&buf_mgr->processing_img_node->node);
						kfree(buf_mgr->processing_img_node->img);
						kfree(buf_mgr->processing_img_node);
					} else
						buf_mgr->dequeue_image(buf_mgr, buf_mgr->processing_img_node);
				}
				break;
			case 1:
				if ((buf_mgr->processing_img_node->img->win_calc_fin == true) &&
						(buf_mgr->processing_img_node->img->mode_select_fin == true) &&
						(buf_mgr->processing_img_node->coll_flag == 0)) {
					if (buf_mgr->processing_img_node->extra_flag == true) {
						buf_mgr->processing_img_node->extra_flag = false;
						__list_del_entry(&buf_mgr->processing_img_node->node);
						eink_free(buf_mgr->processing_img_node->img->vaddr,
								buf_mgr->processing_img_node->img->paddr, buf_size);
						kfree(buf_mgr->processing_img_node->img->eink_hist);
						kfree(buf_mgr->processing_img_node->img);
						kfree(buf_mgr->processing_img_node);
					} else if (buf_mgr->processing_img_node->img->de_bypass_flag == true) {
						buf_mgr->processing_img_node->img->de_bypass_flag = false;
						__list_del_entry(&buf_mgr->processing_img_node->node);
						kfree(buf_mgr->processing_img_node->img);
						kfree(buf_mgr->processing_img_node);
					} else
						buf_mgr->remove_img_from_coll_list(buf_mgr, buf_mgr->processing_img_node);
				}
			case -1:
				EINK_DEFAULT_MSG("maybr init or already merge free\n");
				break;
			default:
				pr_err("unknown update master(%d)\n", buf_mgr->processing_img_node->update_master);
				break;
			}
			EINK_DEFAULT_MSG("Free unused img node\n");
		} else
			EINK_DEFAULT_MSG("First fresh or the Node is NULL\n");

		buf_mgr->processing_img_node = curnode;
		EINK_INFO_MSG("%s: node %p\n", __func__, buf_mgr->processing_img_node);

		first_fresh = false;

		wait_event_interruptible(eink_mgr->upd_pic_accept_queue,
				(eink_mgr->upd_pic_accept_flag == 1));
		/* spin_lock_irqsave();记得后面加个锁 */
		eink_mgr->upd_pic_accept_flag = 0;
		/* spin_unlock_irerestore(); */

	}

	return 0;
err_out:
	return ret;
}

int eink_update_image(struct eink_manager *eink_mgr,
		struct disp_layer_config_inner *config,
		unsigned int layer_num,
		struct eink_upd_cfg *upd_cfg)
{
	int ret = 0;

	EINK_INFO_MSG("func input!\n");

	mutex_lock(&eink_mgr->standby_lock);
	if (eink_mgr->suspend_state) {
		mutex_unlock(&eink_mgr->standby_lock);
		pr_warn("eink is suspend(%d), don't update\n", eink_mgr->suspend_state);
		return 0;
	}
	mutex_unlock(&eink_mgr->standby_lock);

	if (eink_mgr->waveform_init_flag == false) {
		ret = waveform_mgr_init(eink_mgr->wav_path, eink_mgr->panel_info.bit_num);
		if (ret) {
			pr_err("%s:init waveform failed!\n", __func__);
			return ret;
		} else
			eink_mgr->waveform_init_flag = true;
	}
	ret = eink_mgr->eink_mgr_enable(eink_mgr);
	if (ret) {
		pr_err("enable eink mgr failed, ret = %d\n", ret);
		return -1;
	}

	ret = eink_mgr->buf_mgr->queue_image(eink_mgr->buf_mgr, config, layer_num, upd_cfg);

	return ret;

}

int upd_pic_accept_irq_handler(void)
{
	struct eink_manager *mgr = g_eink_manager;

	mgr->upd_pic_accept_flag = 1;
	wake_up_interruptible(&(mgr->upd_pic_accept_queue));

	return 0;
}

int upd_pic_finish_irq_handler(struct buf_manager *mgr)
{
	struct eink_manager *eink_mgr = get_eink_manager();


	eink_mgr->upd_pic_fin_flag = 1;
	wake_up_interruptible(&eink_mgr->upd_pic_queue);

	return 0;
}

#if 0
int upd_coll_win_irq_handler(struct buf_manager *mgr, u32 max_pipe_cnt)
{
	struct eink_manager *eink_mgr = g_eink_manager;
	struct upd_win *win = NULL;
	struct img_node *img_node = NULL;
	u32 status0 = 0;
	u64 status1 = 0;

	img_node = mgr->processing_img_node;
	if (img_node == NULL) {
		pr_err("%s:something wrong!\n", __func__);
	}
	EINK_INFO_MSG("node addr is %p\n", img_node);

	win = &mgr->processing_img_node->img->upd_win;
	eink_get_coll_win(win);

	EINK_INFO_MSG("upd_win: (%d, %d)~(%d, %d)\n", win->left, win->top, win->right, win->bottom);

	status0 = eink_get_coll_status0();
	img_node->coll_flag = status0;


	if (max_pipe_cnt == 64) {
		status1 = eink_get_coll_status1();
		img_node->coll_flag |= (status1 << 32);
	}

	EINK_INFO_MSG("coll status0=0x%x\n, status1=0x%llx, coll flag = 0x%llx",
						status0, status1, img_node->coll_flag);

	/* fix me buf state */

	img_node->img->state = CAN_USED;
	if (img_node->coll_flag) {
		mgr->add_img_to_coll_list(mgr, img_node);
	} else
		pr_err("%s:coll flag = 0 wrong\n", __func__);

	eink_mgr->upd_coll_win_flag = 1;
	wake_up_interruptible(&(eink_mgr->upd_pic_queue));

	return 0;
}
#endif
void upd_coll_win_irq_handler(struct work_struct *work)
{
	struct eink_manager *eink_mgr = get_eink_manager();
	struct buf_manager *mgr = eink_mgr->buf_mgr;
	struct pipe_manager *pipe_mgr = eink_mgr->pipe_mgr;
	struct upd_win *win = NULL;
	struct img_node *img_node = NULL;
	u32 status0 = 0;
	u64 status1 = 0;
	u32 max_pipe_cnt = pipe_mgr->max_pipe_cnt;

	img_node = mgr->processing_img_node;
	if (img_node == NULL) {
		pr_err("%s:something wrong!\n", __func__);
	}
	EINK_INFO_MSG("node addr is %p\n", img_node);

	win = &mgr->processing_img_node->img->upd_win;
	eink_get_coll_win(win);

	EINK_INFO_MSG("upd_win: (%d, %d)~(%d, %d)\n", win->left, win->top, win->right, win->bottom);

	status0 = eink_get_coll_status0();
	img_node->coll_flag = status0;


	if (max_pipe_cnt == 64) {
		status1 = eink_get_coll_status1();
		img_node->coll_flag |= (status1 << 32);
	}

	EINK_INFO_MSG("coll status0=0x%x, status1=0x%llx, coll flag = 0x%llx\n",
					status0, status1, img_node->coll_flag);

	/* fix me buf state */

	img_node->img->state = CAN_USED;
	if (img_node->coll_flag) {
		mgr->add_img_to_coll_list(mgr, img_node);
	} else
		pr_err("%s:coll flag = 0 wrong\n", __func__);

	eink_mgr->upd_coll_win_flag = 1;
	wake_up_interruptible(&(eink_mgr->upd_pic_queue));

	return;
}

int pipe_finish_irq_handler(struct pipe_manager *mgr)
{
	unsigned long flags = 0;
	u32 pipe_cnt = 0, pipe_id = 0;
	u64 finish_val = 0;
	struct pipe_info_node *cur_pipe = NULL, *tmp_pipe = NULL;

	pipe_cnt = mgr->max_pipe_cnt;

	spin_lock_irqsave(&mgr->list_lock, flags);

	finish_val = eink_get_fin_pipe_id();
	EINK_INFO_MSG("Pipe finish val = 0x%llx\n", finish_val);

	for (pipe_id = 0; pipe_id < pipe_cnt; pipe_id++) {
		if ((1ULL << pipe_id) & finish_val) {
			list_for_each_entry_safe(cur_pipe, tmp_pipe, &mgr->pipe_used_list, node) {
				if (cur_pipe->pipe_id != pipe_id) {
					continue;
				}

				if (mgr->release_pipe)
					mgr->release_pipe(mgr, cur_pipe);
				list_move_tail(&cur_pipe->node, &mgr->pipe_free_list);
				break;
			}
		}
	}

	eink_reset_fin_pipe_id(finish_val);

	spin_unlock_irqrestore(&mgr->list_lock, flags);
	return 0;
}

static int eink_intterupt_proc(int irq, void *parg)
{
	struct eink_manager *eink_mgr = NULL;
	struct buf_manager *buf_mgr = NULL;
	struct pipe_manager *pipe_mgr = NULL;
	int reg_val = -1;
	unsigned int ee_fin, upd_pic_accept, upd_pic_fin, pipe_fin, edma_fin, upd_coll_occur;

	eink_mgr = (struct eink_manager *)parg;
	if (eink_mgr == NULL) {
		pr_err("%s:mgr is NULL!\n", __func__);
		return EINK_IRQ_RETURN;
	}

	buf_mgr = eink_mgr->buf_mgr;
	pipe_mgr = eink_mgr->pipe_mgr;
	if ((buf_mgr == NULL) || (pipe_mgr == NULL)) {
		pr_err("buf or pipe mgr is NULL\n");
		return EINK_IRQ_RETURN;
	}

	reg_val = eink_irq_query();
	EINK_INFO_MSG("Enter Interrupt Proc, Reg_Val = 0x%x\n", reg_val);

	upd_pic_accept = reg_val & 0x10;
	upd_pic_fin = reg_val & 0x100;
	pipe_fin = reg_val & 0x1000;
	upd_coll_occur = reg_val & 0x100000;
	ee_fin = reg_val & 0x1000000;
	edma_fin = reg_val & 0x10000000;

	if (upd_pic_accept == 0x10) {
		upd_pic_accept_irq_handler();
	}

	if (upd_pic_fin == 0x100) {
		if (upd_coll_occur == 0x100000) {
			queue_work(buf_mgr->coll_img_workqueue, &buf_mgr->coll_handle_work);
		} else {
			upd_pic_finish_irq_handler(buf_mgr);
		}
	}

	if (ee_fin == 0x1000000) {
		eink_mgr->ee_finish = true;
#ifdef OFFLINE_SINGLE_MODE
		queue_work(pipe_mgr->dec_workqueue, &pipe_mgr->decode_ctrl_work);
#endif
	}

	if (pipe_fin == 0x1000) {
		pipe_finish_irq_handler(pipe_mgr);
	}

	if (edma_fin == 0x10000000) {
#ifdef OFFLINE_SINGLE_MODE
		queue_work(pipe_mgr->edma_workqueue, &pipe_mgr->edma_ctrl_work);
#endif
	}

	return EINK_IRQ_RETURN;
}

#if 0
static int eink_intterupt_proc(int irq, void *parg)
{
	struct eink_manager *eink_mgr = NULL;
	struct buf_manager *buf_mgr = NULL;
	struct pipe_manager *pipe_mgr = NULL;
	int irq_type = -1;

	eink_mgr = (struct eink_manager *)parg;
	if (eink_mgr == NULL) {
		pr_err("%s:mgr is NULL!\n", __func__);
		return EINK_IRQ_RETURN;
	}

	buf_mgr = eink_mgr->buf_mgr;
	pipe_mgr = eink_mgr->pipe_mgr;
	if ((buf_mgr == NULL) || (pipe_mgr == NULL)) {
		pr_err("buf or pipe mgr is NULL\n");
		return EINK_IRQ_RETURN;
	}

	irq_type = eink_irq_query();
	EINK_INFO_MSG("Enter interrupt proc, irq type = %d\n", irq_type);
	switch (irq_type) {
	case 0:/* ee_finish */
		eink_mgr->ee_finish = true;
#ifdef OFFLINE_SINGLE_MODE
		queue_work(pipe_mgr->dec_workqueue, &pipe_mgr->decode_ctrl_work);
#endif
		break;
	case 1:/* upd_pic_accept */
		upd_pic_accept_irq_handler();
		break;
	case 2:/* upd_pic_finish */
		upd_pic_finish_irq_handler(buf_mgr);
		break;
	case 3:/* pipe_fin */
		pipe_finish_irq_handler(pipe_mgr);
		break;
	case 4:/* edma_fin */
#ifdef OFFLINE_SINGLE_MODE
		queue_work(pipe_mgr->edma_workqueue, &pipe_mgr->edma_ctrl_work);
#endif
		break;
	case 5:/* upd_coll_occur */
//		upd_coll_win_irq_handler(buf_mgr, pipe_mgr->max_pipe_cnt);
		queue_work(buf_mgr->coll_img_workqueue, &buf_mgr->coll_handle_work);
		break;
	default:
		pr_err("invalid irq_type %d\n", irq_type);
		break;
	}

	return EINK_IRQ_RETURN;
}
#endif

static int eink_set_temperature(struct eink_manager *mgr, u32 temp)
{
	int ret = 0;

	if (mgr)
		mgr->panel_temperature = temp;
	else {
		pr_err("%s:mgr is NULL!\n", __func__);
		ret = -1;
	}

	return ret;
}

s32 eink_get_resolution(struct eink_manager *mgr, u32 *xres, u32 *yres)
{
	if (mgr == NULL) {
		pr_err("[%s]: eink manager is NULL!\n", __func__);
		return -EINVAL;
	}

	*xres = mgr->panel_info.width;
	*yres = mgr->panel_info.height;
	return 0;
}

u32 eink_get_temperature(struct eink_manager *mgr)
{
	u32 temp = 28;

	if (mgr) {
		temp = mgr->panel_temperature;
	} else
		pr_err("%s: mgr is NULL!\n", __func__);

	return temp;
}

int eink_get_sys_config(struct init_para *para)
{
	int ret = 0, i = 0;
	s32 value = 0;
	char primary_key[20], sub_name[25];
	struct eink_gpio_cfg *gpio_info;

	struct eink_panel_info *panel_info = NULL;
	struct timing_info *timing_info = NULL;

	panel_info = &para->panel_info;
	timing_info = &para->timing_info;

	sprintf(primary_key, "eink");

	/* eink power */
	ret = eink_sys_script_get_item(primary_key, "eink_power", (int *)para->eink_power, 2);
	if (ret == 2) {
		para->power_used = 1;
	}

	/* eink panel gpio */
	for (i = 0; i < EINK_GPIO_NUM; i++) {
		sprintf(sub_name, "eink_gpio_%d", i);

		gpio_info = &(para->eink_gpio[i]);
		ret = eink_sys_script_get_item(primary_key, sub_name,
					     (int *)gpio_info, 3);
		if (ret == 3)
			para->eink_gpio_used[i] = 1;
		EINK_INFO_MSG("eink_gpio_%d used = %d\n", i, para->eink_gpio_used[i]);
	}

	/* eink pin power */
	ret = eink_sys_script_get_item(primary_key, "eink_pin_power", (int *)para->eink_pin_power, 2);

	/* single or dual 0 single 1 dual 2 four*/
	ret = eink_sys_script_get_item(primary_key, "eink_scan_mode", &value, 1);
	if (ret == 1) {
		panel_info->eink_scan_mode = value;
	}
	/* eink panel cfg */
	ret = eink_sys_script_get_item(primary_key, "eink_width", &value, 1);
	if (ret == 1) {
		panel_info->width = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_height", &value, 1);
	if (ret == 1) {
		panel_info->height = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_fresh_hz", &value, 1);
	if (ret == 1) {
		panel_info->fresh_hz = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_gray_level", &value, 1);
	if (ret == 1) {
		panel_info->gray_level_cnt = value;
	}

/*
	ret = disp_sys_script_get_item(primary_key, "eink_sdck", &value, 1);
	if (ret == 1) {
		panel_info->sdck = value;
	}
*/
	ret = disp_sys_script_get_item(primary_key, "eink_bits", &value, 1);
	if (ret == 1) {
		panel_info->bit_num = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_data_len", &value, 1);
	if (ret == 1) {
		panel_info->data_len = value;
	}
	/* eink timing para */
	ret = disp_sys_script_get_item(primary_key, "eink_lsl", &value, 1);
	if (ret == 1) {
		timing_info->lsl = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_lbl", &value, 1);
	if (ret == 1) {
		timing_info->lbl = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_lel", &value, 1);
	if (ret == 1) {
		timing_info->lel = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_gdck_sta", &value, 1);
	if (ret == 1) {
		timing_info->gdck_sta = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_lgonl", &value, 1);
	if (ret == 1) {
		timing_info->lgonl = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_gdoe_start_line", &value, 1);
	if (ret == 1) {
		timing_info->gdoe_start_line = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_fsl", &value, 1);
	if (ret == 1) {
		timing_info->fsl = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_fbl", &value, 1);
	if (ret == 1) {
		timing_info->fbl = value;
	}

	ret = disp_sys_script_get_item(primary_key, "eink_fel", &value, 1);
	if (ret == 1) {
		timing_info->fel = value;
	}

	timing_info->ldl = (panel_info->width) / (panel_info->data_len / 2);
	timing_info->fdl = panel_info->height;

	ret = eink_sys_script_get_item(primary_key, "eink_wav_path", (int *)para->wav_path, 2);
	if (ret != 2) {
		memcpy(para->wav_path, DEFAULT_WAVEFORM_PATH, strnlen(DEFAULT_WAVEFORM_PATH, MAX_PATH_LEN));
		pr_err("[EINK]get eink path fail, set default path=%s\n", para->wav_path);
	}

	return ret;
}

s32 eink_set_global_clean_cnt(struct eink_manager *mgr, u32 cnt)
{
	struct buf_manager *buf_mgr = NULL;
	s32 ret = -1;

	if (mgr == NULL) {
		pr_err("%s:eink manager is null\n", __func__);
		return -1;
	}

	buf_mgr = mgr->buf_mgr;
	if (buf_mgr == NULL) {
		pr_err("%s:buf_mgr is null\n", __func__);
		return -1;
	}

	if (mgr->buf_mgr->set_global_clean_counter)
		ret = mgr->buf_mgr->set_global_clean_counter(buf_mgr, cnt);

	return ret;
}

static void print_panel_info(struct init_para *para)
{
	struct eink_panel_info *info = &para->panel_info;
	struct timing_info *timing = &para->timing_info;

	EINK_INFO_MSG("width=%d, height=%d, fresh_hz=%d, scan_mode=%d\n",
			info->width, info->height, info->fresh_hz, info->eink_scan_mode);
	EINK_INFO_MSG("sdck=%d, bit_num=%d, data_len=%d, gray_level_cnt=%d\n",
			info->sdck, info->bit_num, info->data_len, info->gray_level_cnt);
	EINK_INFO_MSG("lsl=%d, lbl=%d, ldl=%d, lel=%d, gdck_sta=%d, lgonl=%d\n",
			timing->lsl, timing->lbl, timing->ldl, timing->lel,
			timing->gdck_sta, timing->lgonl);
	EINK_INFO_MSG("fsl=%d, fbl=%d, fdl=%d, fel=%d, gdoe_start_line=%d\n",
			timing->fsl, timing->fbl, timing->fdl, timing->fel,
			timing->gdoe_start_line);
	EINK_INFO_MSG("wavdata_path = %s\n", para->wav_path);
}

int eink_clk_enable(struct eink_manager *mgr)
{
	int ret = 0;
	u32 vsync = 0, hsync = 0;
	struct timing_info *timing = NULL;
	u32 fresh_rate = 0;
	unsigned long panel_freq = 0, temp_freq = 0;

	if (mgr->ee_clk) {
		ret = clk_prepare_enable(mgr->ee_clk);
	}

	timing = &mgr->timing_info;
	fresh_rate = mgr->panel_info.fresh_hz;

	hsync = timing->lsl + timing->lbl + timing->ldl + timing->lel;
	vsync = timing->fsl + timing->fbl + timing->fdl + timing->fel;
	panel_freq = fresh_rate * hsync * vsync;

	EINK_INFO_MSG("panel_freq = %lu\n", panel_freq);

	if (mgr->panel_clk) {
		ret = clk_set_rate(mgr->panel_clk, panel_freq);
		if (ret) {
			pr_err("%s:set panel freq failed!\n", __func__);
			return -1;
		}

		temp_freq = clk_get_rate(mgr->panel_clk);
		if (panel_freq != temp_freq) {
			pr_warn("%s: not set real clk, freq=%lu\n", __func__, temp_freq);
		}

		ret = clk_prepare_enable(mgr->panel_clk);
	}

	return ret;
}

void eink_clk_disable(struct eink_manager *mgr)
{
	if (mgr->ee_clk)
		clk_disable(mgr->ee_clk);

	if (mgr->panel_clk)
		clk_disable(mgr->panel_clk);
	return;
}

s32 eink_get_clk_rate(struct clk *device_clk)
{
	unsigned long freq = 0;

	if (device_clk == NULL) {
		pr_err("[%s]: clk is NULL!\n", __func__);
		return -EINVAL;
	}

	freq = clk_get_rate(device_clk);
	EINK_DEFAULT_MSG("clk freq = %ld\n", freq);

	return freq;
}

s32 eink_get_fps(struct eink_manager *mgr)
{
	int fps = 0;

	if (mgr == NULL) {
		pr_err("[%s]: mgr is NULL\n", __func__);
		return -EINVAL;
	}
	fps = mgr->panel_info.fresh_hz;
	return fps;
}

s32 eink_dump_config(struct eink_manager *mgr, char *buf)
{
	u32 count = 0;
	struct buf_manager *buf_mgr = NULL;

	if (mgr == NULL) {
		pr_err("[%s]:NULL hdl!\n", __func__);
		return -EINVAL;
	}

	buf_mgr = mgr->buf_mgr;

	count += sprintf(buf + count, "\timg_order[%3d] ", buf_mgr->upd_order);
#if 0
	count += sprintf(buf + count, "fb[%4u,%4u;%4u,%4u;%4u,%4u] ",
			 data.config.info.fb.size[0].width,
			 data.config.info.fb.size[0].height,
			 data.config.info.fb.size[1].width,
			 data.config.info.fb.size[1].height,
			 data.config.info.fb.size[2].width,
			 data.config.info.fb.size[2].height);
	count += sprintf(buf + count, "crop[%4u,%4u,%4u,%4u] ",
			 (unsigned int)(data.config.info.fb.crop.x >> 32),
			 (unsigned int)(data.config.info.fb.crop.y >> 32),
			 (unsigned int)(data.config.info.fb.crop.width >> 32),
			 (unsigned int)(data.config.info.fb.crop.height >> 32));
	count += sprintf(buf + count, "frame[%4d,%4d,%4u,%4u] ",
			 data.config.info.screen_win.x,
			 data.config.info.screen_win.y,
			 data.config.info.screen_win.width,
			 data.config.info.screen_win.height);
	count += sprintf(buf + count, "addr[%8llx,%8llx,%8llx] ",
			 data.config.info.fb.addr[0],
			 data.config.info.fb.addr[1],
			 data.config.info.fb.addr[2]);
	count += sprintf(buf + count, "flags[0x%8x] trd[%1d,%1d]\n",
			 data.config.info.fb.flags, data.config.info.b_trd_out,
			 data.config.info.out_trd_mode);
#endif
	return count;

}

s32 eink_mgr_enable(struct eink_manager *eink_mgr)
{
	int ret = 0, sel = 0;
	static int first_en = 1;

	struct pipe_manager *pipe_mgr = NULL;
	struct fmt_convert_manager *cvt_mgr = NULL;
	struct timing_ctrl_manager *timing_cmgr = NULL;

	EINK_INFO_MSG("input!\n");

	pipe_mgr = eink_mgr->pipe_mgr;
	cvt_mgr = eink_mgr->convert_mgr;
	timing_cmgr = eink_mgr->timing_ctrl_mgr;

	if ((cvt_mgr == NULL) || (pipe_mgr == NULL) || (timing_cmgr == NULL)) {
		pr_err("%s: eink is not initial\n", __func__);
		return -1;
	}
	mutex_lock(&eink_mgr->enable_lock);
	if (eink_mgr->enable_flag == true) {
		mutex_unlock(&eink_mgr->enable_lock);
		return 0;
	}

	ret = cvt_mgr->enable(sel);
	if (ret) {
		pr_err("%s:enable convert failed\n", __func__);
		return ret;
	}
	if (first_en) {
		eink_clk_enable(eink_mgr);
		first_en = 0;
	}

	ret = pipe_mgr->pipe_mgr_enable(pipe_mgr);
	if (ret) {
		pr_err("%s:fail to enable pipe mgr", __func__);
		goto pipe_enable_fail;
	}

	ret = timing_cmgr->enable(timing_cmgr);
	if (ret) {
		pr_err("%s:fail to enable timing ctrl mgr", __func__);
		goto timing_enable_fail;
	}
#if defined(OFFLINE_MULTI_MODE) || defined(OFFLINE_SINGLE_MODE)
	eink_offline_enable(1);
#endif
	eink_mgr->enable_flag = true;
	mutex_unlock(&eink_mgr->enable_lock);

	return 0;

timing_enable_fail:
	pipe_mgr->pipe_mgr_disable();
pipe_enable_fail:
	cvt_mgr->disable(sel);
	eink_mgr->enable_flag = false;
	mutex_unlock(&eink_mgr->enable_lock);
	return ret;
}

s32 eink_mgr_disable(struct eink_manager *eink_mgr)
{
	int ret = 0, sel = 0;

	struct pipe_manager *pipe_mgr = NULL;
	struct fmt_convert_manager *cvt_mgr = NULL;
	struct timing_ctrl_manager *timing_cmgr = NULL;

	pipe_mgr = eink_mgr->pipe_mgr;
	cvt_mgr = eink_mgr->convert_mgr;
	timing_cmgr = eink_mgr->timing_ctrl_mgr;

	if ((cvt_mgr == NULL) || (pipe_mgr == NULL) || (timing_cmgr == NULL)) {
		pr_err("%s:eink manager is not initial\n", __func__);
		return -1;
	}

	mutex_lock(&eink_mgr->enable_lock);
	if (eink_mgr->enable_flag == false) {
		mutex_unlock(&eink_mgr->enable_lock);
		return 0;
	}

	ret = cvt_mgr->disable(sel);
	if (ret) {
		pr_err("fail to disable converter(%d)\n", ret);
		goto convert_disable_fail;
	}

	ret = pipe_mgr->pipe_mgr_disable();
	if (ret) {
		pr_err("fail to disable pipe(%d)\n", ret);
		goto pipe_disable_fail;
	}

	ret = timing_cmgr->disable(timing_cmgr);
	if (ret) {
		pr_err("%s:fail to enable timing ctrl mgr", __func__);
		goto timing_enable_fail;
	}
#if defined(OFFLINE_MULTI_MODE) || defined(OFFLINE_SINGLE_MODE)
	eink_offline_enable(0);
#endif
	eink_mgr->enable_flag = false;
	mutex_unlock(&eink_mgr->enable_lock);

	return 0;

timing_enable_fail:
	pipe_mgr->pipe_mgr_disable();
pipe_disable_fail:
	cvt_mgr->enable(sel);
convert_disable_fail:
	eink_mgr->enable_flag = true;
	mutex_unlock(&eink_mgr->enable_lock);

	return ret;
}

int eink_mgr_init(struct init_para *para)
{
	int ret = 0;
	int irq_no = 0;
	struct eink_manager *eink_mgr = NULL;

	eink_mgr = (struct eink_manager *)kmalloc(sizeof(struct eink_manager), GFP_KERNEL | __GFP_ZERO);
	if (eink_mgr == NULL) {
		pr_err("%s:malloc mgr failed!\n", __func__);
		ret = -ENOMEM;
		goto eink_mgr_err;
	}

	g_eink_manager = eink_mgr;

	memset(eink_mgr, 0, sizeof(struct eink_manager));

	print_panel_info(para);

	memcpy(&eink_mgr->panel_info, &para->panel_info, sizeof(struct eink_panel_info));
	memcpy(&eink_mgr->timing_info, &para->timing_info, sizeof(struct timing_info));
	memcpy(eink_mgr->wav_path, para->wav_path, WAV_PATH_LEN);
	memcpy(eink_mgr->eink_pin_power, para->eink_pin_power, POWER_STR_LEN);

	eink_mgr->eink_update = eink_update_image;
	eink_mgr->eink_set_global_clean_cnt = eink_set_global_clean_cnt;
	eink_mgr->eink_mgr_enable = eink_mgr_enable;
	eink_mgr->eink_mgr_disable = eink_mgr_disable;

	eink_mgr->set_temperature = eink_set_temperature;
	eink_mgr->get_temperature = eink_get_temperature;
	eink_mgr->get_resolution = eink_get_resolution;
	eink_mgr->get_clk_rate = eink_get_clk_rate;
	eink_mgr->dump_config = eink_dump_config;
	eink_mgr->get_fps = eink_get_fps;

	eink_mgr->upd_pic_accept_flag = 0;
	eink_mgr->panel_temperature = 28;
	eink_mgr->waveform_init_flag = false;
	eink_mgr->enable_flag = false;

	eink_mgr->ee_clk = para->ee_clk;
	eink_mgr->panel_clk = para->panel_clk;

#ifdef VIRTUAL_REGISTER
	test_vreg_base = eink_malloc(0x1ffff, &test_preg_base);
	eink_set_reg_base((void __iomem *)test_vreg_base);
#else
	eink_set_reg_base(para->ee_reg_base);
#endif

	irq_no = para->ee_irq_no;
	ret = request_irq(irq_no, (irq_handler_t)eink_intterupt_proc, 0, "sunxi-eink", (void *)eink_mgr);
	if (ret) {
		pr_err("%s:irq request failed!\n", __func__);
		goto eink_mgr_err;
	}

	eink_mgr->convert_mgr = get_fmt_convert_mgr(0);
	init_waitqueue_head(&eink_mgr->upd_pic_accept_queue);
	init_waitqueue_head(&eink_mgr->upd_pic_queue);
	mutex_init(&eink_mgr->standby_lock);
	mutex_init(&eink_mgr->enable_lock);

	ret = buf_mgr_init(eink_mgr);
	if (ret) {
		pr_err("%s:init buf mgr failed!\n", __func__);
		goto eink_mgr_err;
	}
	ret = index_mgr_init(eink_mgr);
	if (ret) {
		pr_err("%s:init index mgr failed!\n", __func__);
		goto eink_mgr_err;
	}
	ret = pipe_mgr_init(eink_mgr);
	if (ret) {
		pr_err("%s:init pipe mgr failed!\n", __func__);
		goto eink_mgr_err;
	}
	ret = timing_ctrl_mgr_init(para);
	if (ret) {
		pr_err("%s:init timing ctrl mgr failed!\n", __func__);
		goto eink_mgr_err;
	}

	eink_mgr->detect_fresh_task = kthread_create(detect_fresh_thread, (void *)eink_mgr, "eink_fresh_proc");
	if (IS_ERR_OR_NULL(eink_mgr->detect_fresh_task)) {
		pr_err("%s:create fresh thread failed!\n", __func__);
		ret = PTR_ERR(eink_mgr->detect_fresh_task);
		goto eink_mgr_err;
	}

	wake_up_process(eink_mgr->detect_fresh_task);

	return ret;
eink_mgr_err:
	kfree(eink_mgr);
	return ret;
}
