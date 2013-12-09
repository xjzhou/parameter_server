#include "box/container.h"
#include "system/node_group.h"

namespace PS {

DECLARE_bool(is_backup_process);
DECLARE_int32(failed_node_id);

Container::Container(const string& name) :
    name_(name), key_range_(0, kMaxKey), inited_(false) {
  postoffice_ = NULL;
  recv_callback_ = NULL;
  send_callback_ = NULL;
  aggregator_callback_ = NULL;
  cur_time_ = 0;
  max_push_delay_ = kInfDelay;
  max_pull_delay_ = kInfDelay;
}

Container::Container(const string& name, key_t min_key, key_t max_key) :
    Container(name) {
  key_range_.Set(min_key, max_key);
}

void Container::Init(KeyRange whole) {
  postoffice_ = Postoffice::Instance();
  postoffice_->Init();
  postmaster_ = Postmaster::Instance();
  key_range_ = postmaster_->Register(this, whole);
  LL <<" I am " << postmaster_->my_uid()<<" my key range "<< key_range_.start() << key_range_.end();

  if (FLAGS_is_backup_process) {
    ReplicaManager* replica_manager = ReplicaManager::Instance();
    replica_manager->Rescue(this, FLAGS_failed_node_id, 0);
  }
  inited_ = true;
}

void Container::Reply(const Mail& from_other, const Mail& my_reply) {
  Mail to_other = from_other.Reply();
  // simply assume all clients (or servers) has the same behavior
  CHECK_EQ(to_other.flag().push().delta(), my_reply.flag().push().delta());
  // filling the values
  to_other.set_vals(Slice(my_reply.keys(), my_reply.vals(), to_other.keys()));
  CHECK_EQ(to_other.keys().size(), to_other.vals().size());
  postoffice_->Send(to_other);
}

void Container::ReadAll() {
  Mail mail;
  // process all received mails, do not wait
  while (mails_received_.TryTake(&mail)) {
    // LL << my_node().ShortName() << " recved";
    // merge data
    MergeRemoteData(mail);
    if (recv_callback_ != NULL)
    	recv_callback_->Run();

    // aggregator
    aggregator_.Insert(mail);
    time_t time = mail.flag().time();
    if (!aggregator_.Success(time, postmaster_->GetNodeGroup(name())))
    	continue;
    if (aggregator_callback_ != NULL)
    	aggregator_callback_->Run();
    // whether need to send data back?
    bool pull = false;
    Mail my_reply;
    for (auto& it : aggregator_.GetTime(time)) {
      if (it.second.flag().type() & Header::PULL) {
        if (pull == false) {
          my_reply = it.second.Reply();
          GetLocalData(&my_reply);
        }
        Reply(it.second, my_reply);
        pull = true;
      }
    }
    aggregator_.Delete(time);
  }
}

// Status Container::PushToServer(Syncflag flag) {
//   flag.set_recver(NodeGroup::kServers);
//   return Push(flag);
// }

Status Container::Push(const Header& h) { // Future* push_fut, Future* pull_fut) {
  while (!inited_) {
    std::this_thread::sleep_for(seconds(1));
  }
  int time = IncrClock();
  // fill in head
  Header head = h;
  head.set_time(time);
  head.set_name(name_);
  if (head.type() & Header::PULL)
    head.set_type(Header::PUSH_PULL);
  else
    head.set_type(Header::PUSH);
  // check consistency before sending
  push_pool_.Insert(time);
  push_pool_.WaitUntil(time - std::max(1, max_push_delay_));
  if (head.type() & Header::PULL) {
    pull_pool_.Insert(time);
    pull_pool_.WaitUntil(time - std::max(1, max_pull_delay_));
    pull_aggregator_.SetType(time, head.recver());
  }
  // if the receiving queue is not empty, process them first
  ReadAll();
  // send it
  Mail mail(head);
  GetLocalData(&mail);
  postoffice_->Send(mail);
  // check again if max*delay = 0
  if (max_push_delay_==0)
    push_pool_.WaitUntil(time);
  if (max_pull_delay_==0)
    pull_pool_.WaitUntil(time);
  return Status::OK();
}

Status Container::Pull(const Header& flag) { // Future* pull_fut) {
  // TODO
  return Status::OK();
}

// Status Container::PullFromServer(Syncflag flag) {
//   flag.set_recver(NodeGroup::kServers);
//   return Pull(flag);
// }


} // namespace PS