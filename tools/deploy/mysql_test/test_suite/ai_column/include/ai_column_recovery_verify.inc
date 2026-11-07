# Run against the same data directory after an abrupt seekdb restart.
--disable_query_log
--disable_result_log
let $baseline = `select count(*) = 1 from ai_column_recovery.docs where id=1
  and ordinary_vector = [4,3,2,1] and emb = [10,0,0,0] and score=7
  and json_unquote(json_extract(tags, concat(char(36), '.input'))) = 'vec-ten'`;
if (!$baseline) { --die Published AI results or ordinary VECTOR metadata did not survive restart
}
set session ai_column_strict_read = on;
--error 11120
select tags from ai_column_recovery.docs where id=1;
set session ai_column_strict_read = off;
let $retry = 1000;
let $ready = 0;
while ($retry)
{
  let $partial = `select count(*) from ai_column_recovery.docs where id=2
    and (tags is null or score is null or emb is null)
    and (tags is not null or score is not null or emb is not null)`;
  if ($partial) { --die Recovery exposed a partially published generation group
  }
  let $ready = `select count(*) = 1 from ai_column_recovery.docs where id=2
    and score=7 and emb=[0,0,0,0]
    and json_unquote(json_extract(tags, concat(char(36), '.input'))) = 'slow-old vec-zero'`;
  if ($ready) { let $retry = 0; }
  if (!$ready) { --real_sleep 0.1
    dec $retry; }
}
if (!$ready) { --die Expired durable claim was not recovered after restart
}
let $clean = `select count(*) = 0 from oceanbase.__all_ai_column_task task
  join oceanbase.__all_table t on t.table_id = task.table_id
  join oceanbase.__all_database d on d.database_id = t.database_id
  where d.database_name = 'ai_column_recovery' and t.table_name = 'docs'`;
if (!$clean) { --die Recovered generation left durable task rows behind
}
let $ann = `select count(*) = 1 and min(id) = 2 from (select id
  from ai_column_recovery.docs
  order by l2_distance(emb,[0,0,0,0]) approximate limit 1) nearest`;
if (!$ann) { --die Recovered embedding is missing from its persisted vector index
}
update ai_column_recovery.docs set content='vec-twenty' where id=1;
let $retry = 150;
let $ready = 0;
while ($retry)
{
  let $ready = `select count(*) = 1 from ai_column_recovery.docs where id=1
    and score=7 and emb=[20,0,0,0]
    and json_unquote(json_extract(tags, concat(char(36), '.input'))) = 'vec-twenty'`;
  if ($ready) { let $retry = 0; }
  if (!$ready) { --real_sleep 0.1
    dec $retry; }
}
if (!$ready) { --die Restored AI expression or dependency metadata did not regenerate results
}
set session ai_column_strict_read = on;
let $complete = `select count(*) = 2 from ai_column_recovery.docs
  where tags is not null and score is not null and emb is not null`;
if (!$complete) { --die Strict Read rejected recovered complete AI columns
}
set session ai_column_strict_read = off;
drop database ai_column_recovery;
call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('recovery_complete_ep');
call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('recovery_embed_ep');
call DBMS_AI_SERVICE.DROP_AI_MODEL('recovery_complete');
call DBMS_AI_SERVICE.DROP_AI_MODEL('recovery_embed');
--enable_result_log
--echo Recovery, Schema reload, group publication, Strict Read and ANN checks passed
