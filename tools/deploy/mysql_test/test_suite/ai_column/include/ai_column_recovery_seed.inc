# The external runner kills its own seekdb immediately after this fixture exits.
--disable_query_log
--disable_result_log
--exec python3 mysql_test/test_suite/ai_column/include/ai_column_mock.py start 19136
create database ai_column_recovery;
call DBMS_AI_SERVICE.CREATE_AI_MODEL('recovery_complete',
  '{"type":"completion","model_name":"mock-complete"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('recovery_complete_ep',
  '{"ai_model_name":"recovery_complete","url":"http://127.0.0.1:19136/v1/chat/completions","access_key":"recovery-key","provider":"openai","request_model_name":"mock-complete"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL('recovery_embed',
  '{"type":"dense_embedding","model_name":"mock-embed"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('recovery_embed_ep',
  '{"ai_model_name":"recovery_embed","url":"http://127.0.0.1:19136/v1/embeddings","access_key":"recovery-key","provider":"openai","request_model_name":"mock-embed"}');
create table ai_column_recovery.docs(
  id bigint primary key, content varchar(64), ordinary_vector vector(4),
  tags json AI COLUMN (AI_COMPLETE('recovery_complete', content))
    WITH (sync_mode='async', max_retries=3),
  score bigint AI COLUMN (AI_COMPLETE('recovery_complete', content))
    WITH (sync_mode='async', max_retries=3),
  emb vector(4) AI COLUMN (AI_EMBED('recovery_embed', content))
    WITH (sync_mode='async', max_retries=3)
);
create vector index recovery_emb_idx on ai_column_recovery.docs(emb)
  with(distance=l2,type=hnsw,lib=vsag,m=10,ef_construction=12,ef_search=40);
insert into ai_column_recovery.docs(id,content,ordinary_vector)
values(1,'vec-ten',[4,3,2,1]);
let $retry = 150;
let $ready = 0;
while ($retry)
{
  let $ready = `select count(*) = 1 from ai_column_recovery.docs
    where tags is not null and score = 7 and emb = [10,0,0,0]`;
  if ($ready) { let $retry = 0; }
  if (!$ready) { --real_sleep 0.1
    dec $retry; }
}
if (!$ready) { --die Recovery fixture could not publish its baseline row
}
insert into ai_column_recovery.docs(id,content,ordinary_vector)
values(2,'slow-old vec-zero',[1,2,3,4]);
let $retry = 100;
let $claimed = 0;
while ($retry)
{
  let $claimed = `select count(*) > 0 from oceanbase.__all_ai_column_task task
    join oceanbase.__all_table t on t.table_id = task.table_id
    join oceanbase.__all_database d on d.database_id = t.database_id
    where d.database_name = 'ai_column_recovery' and t.table_name = 'docs'
      and task.state = 4`;
  if ($claimed) { let $retry = 0; }
  if (!$claimed) { --real_sleep 0.01
    dec $retry; }
}
if (!$claimed) { --die Recovery fixture did not observe a durable worker claim
}
let $hidden = `select tags is null and score is null and emb is null
  from ai_column_recovery.docs where id=2`;
if (!$hidden) { --die Claimed group was unexpectedly published before the crash
}
--enable_result_log
--echo Recovery fixture observed a durable claim and an unpublished group
